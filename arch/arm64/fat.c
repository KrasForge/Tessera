/* arch/arm64/fat.c - minimal FAT16 reader (Issue #34, M8) */

#include "fat.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Convert "pass.elf" to the 11-byte padded 8.3 form "PASS    ELF". */
static void to_83(const char *name, char out[11])
{
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    for (; name[i] && name[i] != '.' && o < 8; i++) out[o++] = up(name[i]);
    while (name[i] && name[i] != '.') i++;        /* skip overlong base */
    if (name[i] == '.') {
        i++;
        for (int e = 0; name[i] && e < 3; i++, e++) out[8 + e] = up(name[i]);
    }
}

int fat_mount(fat_fs_t *fs, fat_read_block_fn read, void *ctx)
{
    fs->read  = read;
    fs->write = (fat_write_block_fn)0;
    fs->ctx   = ctx;

    uint8_t bs[FAT_SECTOR];
    if (read(ctx, 0, bs) != 0)
        return -1;
    if (bs[510] != 0x55 || bs[511] != 0xAA)       /* boot signature */
        return -2;

    fs->bytes_per_sec = rd16(bs + 0x0B);
    fs->sec_per_clus  = bs[0x0D];
    fs->reserved      = rd16(bs + 0x0E);
    fs->num_fats      = bs[0x10];
    fs->root_entries  = rd16(bs + 0x11);
    fs->sec_per_fat   = rd16(bs + 0x16);

    if (fs->bytes_per_sec != FAT_SECTOR || fs->sec_per_clus == 0 ||
        fs->num_fats == 0 || fs->sec_per_fat == 0 || fs->root_entries == 0)
        return -3;

    fs->fat_start    = fs->reserved;
    fs->root_start   = fs->reserved + (uint32_t)fs->num_fats * fs->sec_per_fat;
    fs->root_sectors = ((uint32_t)fs->root_entries * 32u + FAT_SECTOR - 1) / FAT_SECTOR;
    fs->data_start   = fs->root_start + fs->root_sectors;
    return 0;
}

/* Next cluster in the chain, or >= 0xFFF8 for end-of-chain / bad. */
static uint16_t fat_next(fat_fs_t *fs, uint16_t cluster)
{
    uint32_t off    = (uint32_t)cluster * 2u;
    uint32_t sector = fs->fat_start + off / FAT_SECTOR;
    uint8_t buf[FAT_SECTOR];
    if (fs->read(fs->ctx, sector, buf) != 0)
        return 0xFFFF;
    return rd16(buf + (off % FAT_SECTOR));
}

long fat_read_file(fat_fs_t *fs, const char *name, uint8_t *buf, uint32_t max)
{
    char want[11];
    to_83(name, want);

    /* Scan the root directory for the 8.3 name. */
    uint16_t first = 0;
    uint32_t size  = 0;
    int found = 0;
    for (uint32_t s = 0; s < fs->root_sectors && !found; s++) {
        uint8_t dir[FAT_SECTOR];
        if (fs->read(fs->ctx, fs->root_start + s, dir) != 0)
            return -1;
        for (uint32_t e = 0; e < FAT_SECTOR; e += 32) {
            if (dir[e] == 0x00)                   /* end of directory */
                return -2;
            if (dir[e] == 0xE5 || (dir[e + 11] & 0x0F) == 0x0F)
                continue;                          /* deleted / LFN entry */
            int match = 1;
            for (int i = 0; i < 11; i++)
                if (dir[e + i] != (uint8_t)want[i]) { match = 0; break; }
            if (match) {
                first = rd16(dir + e + 0x1A);
                size  = rd32(dir + e + 0x1C);
                found = 1;
                break;
            }
        }
    }
    if (!found)
        return -2;
    if (size > max)
        return -3;

    /* Follow the cluster chain, copying file bytes. */
    uint32_t clus_bytes = (uint32_t)fs->sec_per_clus * FAT_SECTOR;
    uint32_t copied = 0;
    uint16_t clus = first;
    while (copied < size && clus >= 2 && clus < 0xFFF8) {
        uint32_t base = fs->data_start + (uint32_t)(clus - 2) * fs->sec_per_clus;
        for (uint32_t s = 0; s < fs->sec_per_clus && copied < size; s++) {
            uint8_t sec[FAT_SECTOR];
            if (fs->read(fs->ctx, base + s, sec) != 0)
                return -1;
            uint32_t n = size - copied;
            if (n > FAT_SECTOR) n = FAT_SECTOR;
            for (uint32_t i = 0; i < n; i++) buf[copied + i] = sec[i];
            copied += n;
        }
        (void)clus_bytes;
        clus = fat_next(fs, clus);
    }
    return (long)copied;
}

/* Enumerate the root directory (issue #82): decode each live 8.3 entry into a
 * "NAME.EXT" string and hand it to `cb`.  Returns the number of files, or a
 * negative value on a read error. */
int fat_list(fat_fs_t *fs, void (*cb)(void *ctx, const char *name), void *ctx)
{
    int count = 0;
    for (uint32_t s = 0; s < fs->root_sectors; s++) {
        uint8_t dir[FAT_SECTOR];
        if (fs->read(fs->ctx, fs->root_start + s, dir) != 0)
            return -1;
        for (uint32_t e = 0; e < FAT_SECTOR; e += 32) {
            if (dir[e] == 0x00)                          /* end of directory */
                return count;
            if (dir[e] == 0xE5 || (dir[e + 11] & 0x0F) == 0x0F)
                continue;                                /* deleted / LFN     */
            if (dir[e + 11] & 0x08)
                continue;                                /* volume label      */

            /* Decode the space-padded 8.3 field into "name.ext". */
            char name[13];
            int  k = 0;
            for (int i = 0; i < 8 && dir[e + i] != ' '; i++)
                name[k++] = (char)dir[e + i];
            if (dir[e + 8] != ' ') {
                name[k++] = '.';
                for (int i = 8; i < 11 && dir[e + i] != ' '; i++)
                    name[k++] = (char)dir[e + i];
            }
            name[k] = '\0';
            if (cb)
                cb(ctx, name);
            count++;
        }
    }
    return count;
}

/* ---- write support (Issue #40) ------------------------------------------- */

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void fat_set_writer(fat_fs_t *fs, fat_write_block_fn write) { fs->write = write; }

/* Number of FAT16 entries the FAT can address. */
static uint32_t fat_max_clusters(fat_fs_t *fs)
{
    return (uint32_t)fs->sec_per_fat * FAT_SECTOR / 2u;
}

/* Read a FAT16 entry. */
static uint16_t fat_get(fat_fs_t *fs, uint16_t clus)
{
    uint32_t off = (uint32_t)clus * 2u;
    uint8_t b[FAT_SECTOR];
    if (fs->read(fs->ctx, fs->fat_start + off / FAT_SECTOR, b) != 0)
        return 0xFFFF;
    return rd16(b + off % FAT_SECTOR);
}

/* Write a FAT16 entry into every FAT copy. */
static int fat_set(fat_fs_t *fs, uint16_t clus, uint16_t val)
{
    uint32_t off = (uint32_t)clus * 2u;
    uint32_t sec = fs->fat_start + off / FAT_SECTOR;
    uint8_t b[FAT_SECTOR];
    if (fs->read(fs->ctx, sec, b) != 0)
        return -1;
    wr16(b + off % FAT_SECTOR, val);
    for (uint32_t f = 0; f < fs->num_fats; f++)
        if (fs->write(fs->ctx, sec + f * fs->sec_per_fat, b) != 0)
            return -1;
    return 0;
}

static void fat_free_chain(fat_fs_t *fs, uint16_t first)
{
    uint16_t c = first;
    while (c >= 2 && c < 0xFFF8) {
        uint16_t nxt = fat_get(fs, c);
        fat_set(fs, c, 0);
        c = nxt;
    }
}

/* Allocate a chain of `nclus` free clusters; return the first via *first_out. */
static int fat_alloc_chain(fat_fs_t *fs, uint32_t nclus, uint16_t *first_out)
{
    uint32_t maxc = fat_max_clusters(fs);
    uint16_t prev = 0, first = 0;
    uint32_t got = 0;
    for (uint16_t c = 2; c < maxc && got < nclus; c++) {
        if (fat_get(fs, c) != 0)
            continue;
        if (fat_set(fs, c, 0xFFFF) != 0) return -1;   /* tentative EOF */
        if (prev) { if (fat_set(fs, prev, c) != 0) return -1; }
        else      { first = c; }
        prev = c;
        got++;
    }
    if (got < nclus) { if (first) fat_free_chain(fs, first); return -1; }
    *first_out = first;
    return 0;
}

/* Find the root-dir entry for name83, or the first free slot.  Sets *found. */
static int fat_dir_find(fat_fs_t *fs, const char want[11],
                        uint32_t *sec_out, uint32_t *off_out, int *found)
{
    int free_sec = -1, free_off = -1;
    *found = 0;
    for (uint32_t s = 0; s < fs->root_sectors; s++) {
        uint8_t d[FAT_SECTOR];
        if (fs->read(fs->ctx, fs->root_start + s, d) != 0)
            return -1;
        for (uint32_t e = 0; e < FAT_SECTOR; e += 32) {
            uint8_t c0 = d[e];
            if (c0 == 0x00 || c0 == 0xE5) {
                if (free_sec < 0) { free_sec = (int)(fs->root_start + s); free_off = (int)e; }
                continue;
            }
            if ((d[e + 11] & 0x0F) == 0x0F)
                continue;                            /* LFN */
            int match = 1;
            for (int i = 0; i < 11; i++)
                if (d[e + i] != (uint8_t)want[i]) { match = 0; break; }
            if (match) { *sec_out = fs->root_start + s; *off_out = e; *found = 1; return 0; }
        }
    }
    if (free_sec < 0)
        return -1;                                   /* directory full */
    *sec_out = (uint32_t)free_sec;
    *off_out = (uint32_t)free_off;
    return 0;
}

static void fat_write_dirent(uint8_t *ent, const char want[11],
                             uint16_t first, uint32_t size)
{
    for (int i = 0; i < 11; i++) ent[i] = (uint8_t)want[i];
    ent[0x0B] = 0x20;                                /* archive attribute */
    for (int i = 0x0C; i < 0x1A; i++) ent[i] = 0;
    wr16(ent + 0x1A, first);
    wr32(ent + 0x1C, size);
}

long fat_write_file(fat_fs_t *fs, const char *name, const uint8_t *buf, uint32_t len)
{
    if (!fs->write)
        return FAT_ENOWRITER;

    char want[11];
    to_83(name, want);

    uint32_t clus_bytes = (uint32_t)fs->sec_per_clus * FAT_SECTOR;
    uint32_t nclus = (len + clus_bytes - 1) / clus_bytes;
    if (nclus == 0) nclus = 1;

    uint32_t dsec, doff; int found;
    if (fat_dir_find(fs, want, &dsec, &doff, &found) != 0)
        return FAT_ENOSPACE;

    if (found) {
        uint8_t d[FAT_SECTOR];
        if (fs->read(fs->ctx, dsec, d) != 0) return FAT_EIO;
        fat_free_chain(fs, rd16(d + doff + 0x1A));
    }

    uint16_t first;
    if (fat_alloc_chain(fs, nclus, &first) != 0)
        return FAT_ENOSPACE;

    uint16_t c = first;
    uint32_t copied = 0;
    while (c >= 2 && c < 0xFFF8 && copied < len) {
        uint32_t base = fs->data_start + (uint32_t)(c - 2) * fs->sec_per_clus;
        for (uint32_t s = 0; s < fs->sec_per_clus && copied < len; s++) {
            uint8_t sec[FAT_SECTOR];
            for (int i = 0; i < FAT_SECTOR; i++) sec[i] = 0;
            uint32_t n = len - copied;
            if (n > FAT_SECTOR) n = FAT_SECTOR;
            for (uint32_t i = 0; i < n; i++) sec[i] = buf[copied + i];
            if (fs->write(fs->ctx, base + s, sec) != 0) return FAT_EIO;
            copied += n;
        }
        c = fat_get(fs, c);
    }

    uint8_t d[FAT_SECTOR];
    if (fs->read(fs->ctx, dsec, d) != 0) return FAT_EIO;
    fat_write_dirent(d + doff, want, first, len);
    if (fs->write(fs->ctx, dsec, d) != 0) return FAT_EIO;
    return (long)len;
}

int fat_rename(fat_fs_t *fs, const char *from, const char *to)
{
    if (!fs->write)
        return FAT_ENOWRITER;

    char wf[11], wt[11];
    to_83(from, wf);
    to_83(to, wt);

    uint32_t fsec, foff; int ffound;
    if (fat_dir_find(fs, wf, &fsec, &foff, &ffound) != 0 || !ffound)
        return FAT_ENOENT;
    uint8_t df[FAT_SECTOR];
    if (fs->read(fs->ctx, fsec, df) != 0) return FAT_EIO;
    uint16_t first = rd16(df + foff + 0x1A);
    uint32_t size  = rd32(df + foff + 0x1C);

    uint32_t tsec, toff; int tfound;
    if (fat_dir_find(fs, wt, &tsec, &toff, &tfound) != 0)
        return FAT_ENOSPACE;
    if (tfound) {
        uint8_t dt[FAT_SECTOR];
        if (fs->read(fs->ctx, tsec, dt) != 0) return FAT_EIO;
        fat_free_chain(fs, rd16(dt + toff + 0x1A));
    }

    uint8_t dt[FAT_SECTOR];
    if (fs->read(fs->ctx, tsec, dt) != 0) return FAT_EIO;
    fat_write_dirent(dt + toff, wt, first, size);
    if (fs->write(fs->ctx, tsec, dt) != 0) return FAT_EIO;

    /* Re-read the source sector (it may equal the target sector) and delete. */
    if (fs->read(fs->ctx, fsec, df) != 0) return FAT_EIO;
    df[foff] = 0xE5;
    if (fs->write(fs->ctx, fsec, df) != 0) return FAT_EIO;
    return FAT_OK;
}
