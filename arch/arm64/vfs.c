/* arch/arm64/vfs.c - path resolver + writable store (Issue #34, ext. #40) */

#include "vfs.h"
#include "fat.h"

static int streq(const char *a, const char *b)
{
    for (int i = 0; i < 96; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

/* If `path` begins with `prefix`, return the remainder, else NULL. */
static const char *after(const char *path, const char *prefix)
{
    int i = 0;
    for (; prefix[i]; i++)
        if (path[i] != prefix[i]) return (const char *)0;
    return path + i;
}

/* The ramdisk/store leaf name for a path: after "/rd/", or the bare path. */
static const char *rd_leaf(const char *path)
{
    const char *n = after(path, "/rd/");
    return n ? n : path;
}

void vfs_init(vfs_t *v)
{
    v->n_rd = 0;
    v->fat  = (fat_fs_t *)0;
    for (int i = 0; i < VFS_STORE_SLOTS; i++)
        v->store[i].used = 0;
}

int vfs_add_ramdisk(vfs_t *v, const char *name, const uint8_t *blob, uint32_t len)
{
    if (v->n_rd >= VFS_MAX_RAMDISK)
        return -1;
    v->rd[v->n_rd].name = name;
    v->rd[v->n_rd].blob = blob;
    v->rd[v->n_rd].len  = len;
    v->n_rd++;
    return 0;
}

void vfs_mount_sd(vfs_t *v, fat_fs_t *fat)
{
    v->fat = fat;
}

/* ---- writable store ------------------------------------------------------ */

static vfs_store_entry_t *store_find(vfs_t *v, const char *name)
{
    for (int i = 0; i < VFS_STORE_SLOTS; i++)
        if (v->store[i].used && streq(v->store[i].name, name))
            return &v->store[i];
    return (vfs_store_entry_t *)0;
}

static vfs_store_entry_t *store_alloc(vfs_t *v, const char *name)
{
    vfs_store_entry_t *e = store_find(v, name);
    if (e) return e;
    for (int i = 0; i < VFS_STORE_SLOTS; i++)
        if (!v->store[i].used) {
            int k = 0;
            for (; name[k] && k < VFS_NAME_MAX - 1; k++)
                v->store[i].name[k] = name[k];
            if (name[k]) return (vfs_store_entry_t *)0;   /* name too long */
            v->store[i].name[k] = '\0';
            v->store[i].used = 1;
            v->store[i].len  = 0;
            return &v->store[i];
        }
    return (vfs_store_entry_t *)0;
}

/* ---- read ---------------------------------------------------------------- */

static long ramdisk_lookup(vfs_t *v, const char *name, const uint8_t **out)
{
    vfs_store_entry_t *s = store_find(v, name);      /* writable store first */
    if (s) { *out = s->buf; return (long)s->len; }
    for (int i = 0; i < v->n_rd; i++)                /* then const registry  */
        if (streq(v->rd[i].name, name)) {
            *out = v->rd[i].blob;
            return (long)v->rd[i].len;
        }
    return -1;
}

long vfs_resolve(vfs_t *v, const char *path, const uint8_t **out,
                 uint8_t *scratch, uint32_t scratch_max)
{
    const char *name;

    if ((name = after(path, "/sd/")) != (const char *)0) {
        if (!v->fat || !scratch)
            return -1;
        long n = fat_read_file(v->fat, name, scratch, scratch_max);
        if (n < 0)
            return n;
        *out = scratch;
        return n;
    }

    return ramdisk_lookup(v, rd_leaf(path), out);
}

/* ---- write / rename ------------------------------------------------------ */

int vfs_write(vfs_t *v, const char *path, const uint8_t *data, uint32_t len)
{
    const char *name;

    if ((name = after(path, "/sd/")) != (const char *)0) {
        if (!v->fat)
            return VFS_ENOENT;
        return fat_write_file(v->fat, name, data, len) < 0 ? VFS_EIO : VFS_OK;
    }

    if (len > VFS_STORE_CAP)
        return VFS_ENOSPACE;
    vfs_store_entry_t *e = store_alloc(v, rd_leaf(path));
    if (!e)
        return VFS_ENOSPACE;
    for (uint32_t i = 0; i < len; i++)
        e->buf[i] = data[i];
    e->len = len;
    return VFS_OK;
}

int vfs_rename(vfs_t *v, const char *from, const char *to)
{
    const char *fs = after(from, "/sd/");
    const char *ts = after(to, "/sd/");
    if (fs && ts) {
        if (!v->fat)
            return VFS_ENOENT;
        return fat_rename(v->fat, fs, ts) < 0 ? VFS_EIO : VFS_OK;
    }

    /* Store rename: move `from` onto `to`, replacing any existing `to`. */
    vfs_store_entry_t *src = store_find(v, rd_leaf(from));
    if (!src)
        return VFS_ENOENT;
    vfs_store_entry_t *dst = store_alloc(v, rd_leaf(to));
    if (!dst)
        return VFS_ENOSPACE;
    if (dst != src) {
        for (uint32_t i = 0; i < src->len; i++)
            dst->buf[i] = src->buf[i];
        dst->len = src->len;
        src->used = 0;
    }
    return VFS_OK;
}
