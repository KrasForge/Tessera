/* arch/arm64/vfs.h - tiny path resolver + writable store for plugin sources and
 * patch files (Issue #34, extended in #40).
 *
 * Resolves a path to bytes from one of three sources:
 *
 *   "/sd/<name>"  -> the FAT16/32 volume (arch/arm64/fat.c)
 *   "/rd/<name>"  -> the in-memory ramdisk (const boot registry, zero-copy)
 *   "<name>"      -> ramdisk (the bare-name form used since M5)
 *
 * For patch persistence (#40) the ramdisk side also has a small WRITABLE store,
 * and the SD side gains write/rename, so vfs_write()/vfs_rename() work the same
 * whether patches live on the SD card or (for testing) in the ramdisk.
 */

#ifndef ARM64_VFS_H
#define ARM64_VFS_H

#include "fat.h"
#include <stdint.h>

#define VFS_MAX_RAMDISK 16

/* Writable in-memory store (patch files during testing, and the atomic-save
 * temp file).  Small: patches are a few hundred bytes. */
#define VFS_STORE_SLOTS 4
#define VFS_STORE_CAP   4096
#define VFS_NAME_MAX    32

typedef struct {
    const char    *name;
    const uint8_t *blob;
    uint32_t       len;
} vfs_rd_entry_t;

typedef struct {
    int      used;
    char     name[VFS_NAME_MAX];
    uint32_t len;
    uint8_t  buf[VFS_STORE_CAP];
} vfs_store_entry_t;

typedef struct {
    vfs_rd_entry_t    rd[VFS_MAX_RAMDISK];
    int               n_rd;
    vfs_store_entry_t store[VFS_STORE_SLOTS];
    fat_fs_t         *fat;       /* mounted SD volume, or NULL */
} vfs_t;

void vfs_init(vfs_t *v);

/* Register a (read-only) ramdisk file. */
int vfs_add_ramdisk(vfs_t *v, const char *name, const uint8_t *blob, uint32_t len);

/* Mount the SD-card FAT volume (already fat_mount()ed). */
void vfs_mount_sd(vfs_t *v, fat_fs_t *fat);

/* Resolve `path` to bytes.  A ramdisk/const file points `*out` at the stored
 * bytes (no copy); an SD file is read into `scratch` (capacity `scratch_max`)
 * and `*out` points there.  Returns the byte count, or negative if unknown. */
long vfs_resolve(vfs_t *v, const char *path, const uint8_t **out,
                 uint8_t *scratch, uint32_t scratch_max);

/* Errors (negative). */
#define VFS_OK        0
#define VFS_ENOENT  (-1)
#define VFS_ENOSPACE (-2)
#define VFS_EIO     (-3)

/* Write `len` bytes to `path`, creating or replacing it.  "/rd/<name>" and a
 * bare name go to the writable store; "/sd/<name>" goes to the FAT volume.
 * Returns VFS_OK or a negative VFS_E*. */
int vfs_write(vfs_t *v, const char *path, const uint8_t *data, uint32_t len);

/* Rename `from` to `to` (same backend), replacing any existing `to`.  Used for
 * atomic save (write temp, then rename over the target). */
int vfs_rename(vfs_t *v, const char *from, const char *to);

#endif /* ARM64_VFS_H */
