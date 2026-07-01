/* arch/arm64/fat.h - minimal FAT16 reader (Issue #34, M8)
 *
 * Just enough FAT16 to load a plugin binary from an SD card: mount a volume
 * over a 512-byte block-read callback, find a file in the root directory by its
 * 8.3 name, and read its contents by following the cluster chain.  Read-only
 * and bounds-checked.  The block source is injected (the SD/EMMC driver on
 * hardware, an in-memory image in the tests), so the parser is unit-tested on
 * the host.
 */

#ifndef ARM64_FAT_H
#define ARM64_FAT_H

#include <stdint.h>

#define FAT_SECTOR 512u

/* Read the 512-byte sector at `lba` into `buf`.  Returns 0 on success. */
typedef int (*fat_read_block_fn)(void *ctx, uint32_t lba, uint8_t *buf);

/* Write the 512-byte sector at `lba` from `buf`.  Returns 0 on success.
 * Optional: set via fat_set_writer() to enable fat_write_file/fat_rename. */
typedef int (*fat_write_block_fn)(void *ctx, uint32_t lba, const uint8_t *buf);

typedef struct {
    fat_read_block_fn  read;
    fat_write_block_fn write;     /* NULL unless fat_set_writer() called */
    void    *ctx;
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t reserved;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t sec_per_fat;
    uint32_t fat_start;       /* first FAT sector            */
    uint32_t root_start;      /* first root-dir sector       */
    uint32_t root_sectors;    /* root-dir size in sectors    */
    uint32_t data_start;      /* first data sector (cluster 2) */
} fat_fs_t;

/* Mount the FAT16 volume reachable through `read`/`ctx`.  Returns 0 on success,
 * a negative value if the boot sector is not a usable FAT16. */
int fat_mount(fat_fs_t *fs, fat_read_block_fn read, void *ctx);

/* Read the root-directory file named `name` (e.g. "PASS.ELF", case-insensitive)
 * into `buf` (capacity `max`).  Returns the file size, or a negative value if
 * the file is missing or does not fit. */
long fat_read_file(fat_fs_t *fs, const char *name, uint8_t *buf, uint32_t max);

/* Enable writing by supplying a block-write callback (issue #40). */
void fat_set_writer(fat_fs_t *fs, fat_write_block_fn write);

/* Create or overwrite the root-directory file `name` with `len` bytes from
 * `buf`.  Allocates a fresh cluster chain, updates the FAT and directory entry,
 * and frees any previous chain.  Returns `len`, or a negative value on error
 * (no writer, out of space, directory full). */
long fat_write_file(fat_fs_t *fs, const char *name, const uint8_t *buf, uint32_t len);

/* Rename `from` to `to` in the root directory, replacing any existing `to`
 * (freeing its chain).  Used for atomic save.  Returns 0 or a negative error. */
int fat_rename(fat_fs_t *fs, const char *from, const char *to);

/* Errors. */
#define FAT_OK        0
#define FAT_ENOWRITER (-10)
#define FAT_ENOSPACE  (-11)
#define FAT_ENOENT    (-12)
#define FAT_EIO       (-13)

#endif /* ARM64_FAT_H */
