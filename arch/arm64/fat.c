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
    fs->read = read;
    fs->ctx  = ctx;

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
