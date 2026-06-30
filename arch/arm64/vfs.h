/* arch/arm64/vfs.h - tiny path resolver for plugin sources (Issue #34, M8)
 *
 * Resolves a plugin path to its ELF bytes from one of two sources, so the
 * loader works identically whether a plugin shipped in the boot ramdisk or was
 * dropped onto the SD card at runtime:
 *
 *   "/sd/<name>"  -> the FAT16 volume (arch/arm64/fat.c), read into a buffer
 *   "/rd/<name>"  -> the in-memory ramdisk registry (zero-copy)
 *   "<name>"      -> ramdisk (the bare-name form used since M5)
 */

#ifndef ARM64_VFS_H
#define ARM64_VFS_H

#include "fat.h"
#include <stdint.h>

#define VFS_MAX_RAMDISK 16

typedef struct {
    const char    *name;
    const uint8_t *blob;
    uint32_t       len;
} vfs_rd_entry_t;

typedef struct {
    vfs_rd_entry_t rd[VFS_MAX_RAMDISK];
    int            n_rd;
    fat_fs_t      *fat;       /* mounted SD volume, or NULL */
} vfs_t;

void vfs_init(vfs_t *v);

/* Register a ramdisk file. */
int vfs_add_ramdisk(vfs_t *v, const char *name, const uint8_t *blob, uint32_t len);

/* Mount the SD-card FAT volume (already fat_mount()ed). */
void vfs_mount_sd(vfs_t *v, fat_fs_t *fat);

/* Resolve `path` to ELF bytes.  For a ramdisk file this points `*out` at the
 * registered blob (no copy); for an SD file it reads into `scratch`
 * (capacity `scratch_max`) and points `*out` there.  Returns the byte count,
 * or a negative value if the path is unknown / unreadable. */
long vfs_resolve(vfs_t *v, const char *path, const uint8_t **out,
                 uint8_t *scratch, uint32_t scratch_max);

#endif /* ARM64_VFS_H */
