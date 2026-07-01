/* tests/arm64/fat_test.c - host unit tests for the FAT16 reader (Issue #34).
 *
 * Builds a tiny in-memory FAT16 image (boot sector + one FAT + root directory +
 * a two-cluster file), mounts it through the block-read callback, and checks
 * that the file is found by its 8.3 name and read back byte-for-byte across the
 * cluster chain - plus rejection of a missing file and an over-capacity read.
 *
 * Build/run via:  make test-arm-fat
 */

#include "fat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* boot, FAT, root, then data clusters (extra clusters left free for writing). */
#define NSECT 16
static uint8_t g_img[NSECT * FAT_SECTOR];

#define FILE_SIZE 600u           /* spans two 512-byte clusters */

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void build_image(void)
{
    memset(g_img, 0, sizeof(g_img));
    uint8_t *boot = g_img;
    put16(boot + 0x0B, 512);     /* bytes/sector  */
    boot[0x0D] = 1;              /* sectors/cluster */
    put16(boot + 0x0E, 1);       /* reserved      */
    boot[0x10] = 1;              /* num FATs      */
    put16(boot + 0x11, 16);      /* root entries  */
    put16(boot + 0x16, 1);       /* sectors/FAT   */
    boot[510] = 0x55; boot[511] = 0xAA;

    uint8_t *fat = g_img + 1 * FAT_SECTOR;
    put16(fat + 2 * 2, 3);        /* cluster 2 -> 3   */
    put16(fat + 3 * 2, 0xFFFF);   /* cluster 3 -> EOF */

    uint8_t *root = g_img + 2 * FAT_SECTOR;
    memcpy(root, "PASS    ELF", 11);
    root[0x0B] = 0x20;                       /* archive attr */
    put16(root + 0x1A, 2);                   /* first cluster */
    put32(root + 0x1C, FILE_SIZE);           /* size          */

    uint8_t *data = g_img + 3 * FAT_SECTOR;  /* clusters 2,3 contiguous */
    for (uint32_t i = 0; i < FILE_SIZE; i++)
        data[i] = (uint8_t)(i * 7 + 1);      /* known pattern */
}

static int read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    if (lba >= NSECT)
        return -1;
    memcpy(buf, g_img + lba * FAT_SECTOR, FAT_SECTOR);
    return 0;
}

static int write_block(void *ctx, uint32_t lba, const uint8_t *buf)
{
    (void)ctx;
    if (lba >= NSECT)
        return -1;
    memcpy(g_img + lba * FAT_SECTOR, buf, FAT_SECTOR);
    return 0;
}

int main(void)
{
    printf("=== Tessera FAT16 reader tests (issue #34) ===\n");
    build_image();

    fat_fs_t fs;
    CHECK(fat_mount(&fs, read_block, 0) == 0, "mount the FAT16 volume");
    CHECK(fs.root_start == 2 && fs.data_start == 3, "geometry computed");

    uint8_t out[1024];
    long n = fat_read_file(&fs, "pass.elf", out, sizeof(out));
    CHECK(n == (long)FILE_SIZE, "read the file (correct size, lower-case name)");
    int ok = 1;
    for (uint32_t i = 0; i < FILE_SIZE; i++)
        if (out[i] != (uint8_t)(i * 7 + 1)) ok = 0;
    CHECK(ok, "file bytes are intact across the two-cluster chain");

    CHECK(fat_read_file(&fs, "nope.elf", out, sizeof(out)) < 0, "missing file rejected");
    CHECK(fat_read_file(&fs, "pass.elf", out, 100) < 0, "over-capacity read rejected");

    /* ---- write support (issue #40) ---- */
    CHECK(fat_write_file(&fs, "patch.txt", (const uint8_t *)"x", 1) == FAT_ENOWRITER,
          "write without a writer is rejected");
    fat_set_writer(&fs, write_block);

    uint8_t big[700];
    for (uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 3 + 5);
    CHECK(fat_write_file(&fs, "patch.txt", big, sizeof(big)) == (long)sizeof(big),
          "write a new 700-byte file (spans two clusters)");
    long rn = fat_read_file(&fs, "patch.txt", out, sizeof(out));
    int rok = (rn == (long)sizeof(big));
    for (uint32_t i = 0; i < sizeof(big); i++) if (out[i] != big[i]) rok = 0;
    CHECK(rok, "read the written file back byte-for-byte");
    /* The original file is untouched by the new allocation. */
    CHECK(fat_read_file(&fs, "pass.elf", out, sizeof(out)) == (long)FILE_SIZE,
          "pre-existing file still intact after writing another");

    /* Overwrite with a shorter payload. */
    CHECK(fat_write_file(&fs, "patch.txt", big, 100) == 100, "overwrite (shorter)");
    CHECK(fat_read_file(&fs, "patch.txt", out, sizeof(out)) == 100, "overwritten size is 100");

    /* Atomic-save primitive: write temp, then rename over the target. */
    CHECK(fat_write_file(&fs, "patch.tmp", big, 300) == 300, "write temp file");
    CHECK(fat_rename(&fs, "patch.tmp", "patch.txt") == FAT_OK, "rename temp -> target");
    CHECK(fat_read_file(&fs, "patch.txt", out, sizeof(out)) == 300, "target now has temp's data");
    CHECK(fat_read_file(&fs, "patch.tmp", out, sizeof(out)) < 0, "temp name gone after rename");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
