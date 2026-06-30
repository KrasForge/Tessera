/* tests/arm64/vfs_test.c - host unit tests for the plugin VFS (Issue #34).
 *
 * Exercises the path resolver in arch/arm64/vfs.c against both of its sources:
 * the in-memory ramdisk (zero-copy, reached by a bare name or "/rd/<name>") and
 * a mounted FAT16 "SD" volume (read into a scratch buffer, reached by
 * "/sd/<name>").  The SD side reuses the same in-memory FAT16 image shape as
 * fat_test.c so the two stay consistent.
 *
 * Build/run via:  make test-arm-vfs
 */

#include "vfs.h"
#include "fat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- a tiny FAT16 image holding one file "SDPLUG.ELF" ---- */
#define NSECT 5
static uint8_t g_img[NSECT * FAT_SECTOR];
#define SD_FILE_SIZE 600u                 /* spans two 512-byte clusters */

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void build_image(void)
{
    memset(g_img, 0, sizeof(g_img));
    uint8_t *boot = g_img;
    put16(boot + 0x0B, 512);
    boot[0x0D] = 1;
    put16(boot + 0x0E, 1);
    boot[0x10] = 1;
    put16(boot + 0x11, 16);
    put16(boot + 0x16, 1);
    boot[510] = 0x55; boot[511] = 0xAA;

    uint8_t *fat = g_img + 1 * FAT_SECTOR;
    put16(fat + 2 * 2, 3);
    put16(fat + 3 * 2, 0xFFFF);

    uint8_t *root = g_img + 2 * FAT_SECTOR;
    memcpy(root, "SDPLUG  ELF", 11);
    root[0x0B] = 0x20;
    put16(root + 0x1A, 2);
    put32(root + 0x1C, SD_FILE_SIZE);

    uint8_t *data = g_img + 3 * FAT_SECTOR;
    for (uint32_t i = 0; i < SD_FILE_SIZE; i++)
        data[i] = (uint8_t)(i * 7 + 1);
}

static int read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    if (lba >= NSECT)
        return -1;
    memcpy(buf, g_img + lba * FAT_SECTOR, FAT_SECTOR);
    return 0;
}

/* ---- a stand-in ramdisk blob ---- */
static const uint8_t g_rd_blob[] = { 'R', 'D', 'P', 'L', 'U', 'G', 0, 1, 2, 3 };

int main(void)
{
    printf("=== Tessera plugin VFS tests (issue #34) ===\n");
    build_image();

    vfs_t v;
    vfs_init(&v);
    CHECK(vfs_add_ramdisk(&v, "pass", g_rd_blob, sizeof(g_rd_blob)) == 0,
          "register a ramdisk blob");

    const uint8_t *out = (const uint8_t *)0;

    /* Ramdisk: bare name and "/rd/" prefix both resolve, zero-copy. */
    long n = vfs_resolve(&v, "pass", &out, (uint8_t *)0, 0);
    CHECK(n == (long)sizeof(g_rd_blob) && out == g_rd_blob,
          "bare name resolves to the ramdisk blob (zero-copy)");
    out = (const uint8_t *)0;
    n = vfs_resolve(&v, "/rd/pass", &out, (uint8_t *)0, 0);
    CHECK(n == (long)sizeof(g_rd_blob) && out == g_rd_blob,
          "\"/rd/<name>\" resolves to the same blob");

    /* Unknown names are rejected. */
    CHECK(vfs_resolve(&v, "nope", &out, (uint8_t *)0, 0) < 0,
          "unknown ramdisk name rejected");
    CHECK(vfs_resolve(&v, "/sd/x.elf", &out, (uint8_t *)0, 0) < 0,
          "\"/sd/\" path with no SD mounted rejected");

    /* Mount the SD volume and resolve a file from it into scratch. */
    fat_fs_t fs;
    CHECK(fat_mount(&fs, read_block, 0) == 0, "mount the FAT16 SD volume");
    vfs_mount_sd(&v, &fs);

    uint8_t scratch[SD_FILE_SIZE + 64];
    out = (const uint8_t *)0;
    n = vfs_resolve(&v, "/sd/sdplug.elf", &out, scratch, sizeof(scratch));
    CHECK(n == (long)SD_FILE_SIZE && out == scratch,
          "\"/sd/<name>\" reads the file into scratch");
    int ok = 1;
    for (uint32_t i = 0; i < SD_FILE_SIZE; i++)
        if (scratch[i] != (uint8_t)(i * 7 + 1)) ok = 0;
    CHECK(ok, "SD file bytes are intact across the cluster chain");

    CHECK(vfs_resolve(&v, "/sd/missing.elf", &out, scratch, sizeof(scratch)) < 0,
          "missing SD file rejected");
    CHECK(vfs_resolve(&v, "/sd/sdplug.elf", &out, scratch, 100) < 0,
          "SD read into too-small scratch rejected");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
