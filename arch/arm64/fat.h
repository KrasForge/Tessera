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

typedef struct {
    fat_read_block_fn read;
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

#endif /* ARM64_FAT_H */
