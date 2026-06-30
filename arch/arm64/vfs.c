/* arch/arm64/vfs.c - tiny path resolver for plugin sources (Issue #34, M8) */

#include "vfs.h"
#include "fat.h"

static int streq(const char *a, const char *b)
{
    for (int i = 0; i < 64; i++) {
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

void vfs_init(vfs_t *v)
{
    v->n_rd = 0;
    v->fat  = (fat_fs_t *)0;
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

static long ramdisk_lookup(vfs_t *v, const char *name, const uint8_t **out)
{
    for (int i = 0; i < v->n_rd; i++)
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

    if ((name = after(path, "/rd/")) != (const char *)0)
        return ramdisk_lookup(v, name, out);

    return ramdisk_lookup(v, path, out);     /* bare name -> ramdisk */
}
