/* tests/arm64/virt/sd_main.c - M8 SD/FAT plugin loading on QEMU 'virt'
 * (Issue #34).
 *
 * Proves the loader hosts a plugin binary that was NOT compiled into the kernel
 * image: it is placed on a FAT16 "SD card" (an in-RAM volume here, the SD/EMMC
 * driver on hardware) and loaded by path through the full validate -> map ->
 * handshake pipeline, in an isolated address space, exactly like a ramdisk
 * plugin.  Covers every acceptance criterion of issue #34:
 *
 *   1. sys_plugin_load("/sd/pass.elf") loads a plugin from the SD card.
 *   2. The same loader loads a plugin from the ramdisk.
 *   3. A wrong-ABI plugin is rejected (PM_EABI) before any of its code runs.
 *   4. A plugin importing a symbol outside the ABI is rejected (PM_EIMPORT).
 *   5. A missing path is rejected (PM_ENOENT).
 *   6. Load/unload from the SD source is leak-free (free pages return to base).
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_mgr.h"
#include "graph_control.h"
#include "fat.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char pass_elf_start[],      pass_elf_end[];
extern char badabi_elf_start[],    badabi_elf_end[];
extern char badimport_elf_start[], badimport_elf_end[];

/* ---- in-RAM FAT16 "SD card" ------------------------------------------------
 * sec_per_clus = 1, one reserved sector (boot), a two-sector FAT (big enough to
 * map every data cluster of two ~66 KiB plugin ELFs), one root-dir sector, then
 * data clusters starting at cluster 2.  Files are laid out contiguously. */
#define SD_SEC_PER_FAT 2u
#define SD_SECTORS 384
static uint8_t g_sd[SD_SECTORS * FAT_SECTOR];
static uint32_t g_next_clus = 2;     /* next free cluster to hand out */

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *sd_sector(uint32_t lba) { return g_sd + (size_t)lba * FAT_SECTOR; }

/* Geometry constants matching the boot sector written by sd_format():
 * reserved(1) + FAT(SD_SEC_PER_FAT) + root(1) + data. */
#define SD_FAT_LBA   1u
#define SD_ROOT_LBA  (1u + SD_SEC_PER_FAT)
#define SD_DATA_LBA  (SD_ROOT_LBA + 1u)

static void sd_format(void)
{
    for (size_t i = 0; i < sizeof(g_sd); i++) g_sd[i] = 0;
    uint8_t *boot = sd_sector(0);
    put16(boot + 0x0B, 512);              /* bytes/sector   */
    boot[0x0D] = 1;                      /* sectors/cluster */
    put16(boot + 0x0E, 1);                /* reserved sectors */
    boot[0x10] = 1;                      /* number of FATs  */
    put16(boot + 0x11, 16);               /* root entries    */
    put16(boot + 0x16, SD_SEC_PER_FAT);   /* sectors/FAT     */
    boot[510] = 0x55; boot[511] = 0xAA;
    g_next_clus = 2;
}

static int next_root_slot(void)
{
    uint8_t *root = sd_sector(SD_ROOT_LBA);
    for (int e = 0; e < 16; e++)
        if (root[e * 32] == 0x00) return e;
    return -1;
}

/* Place `len` bytes under the 11-byte 8.3 `name83` on the SD image. */
static void sd_add_file(const char *name83, const uint8_t *bytes, uint32_t len)
{
    uint32_t clusters = (len + FAT_SECTOR - 1) / FAT_SECTOR;
    if (clusters == 0) clusters = 1;
    uint32_t first = g_next_clus;

    uint8_t *fat = sd_sector(SD_FAT_LBA);
    for (uint32_t c = 0; c < clusters; c++) {
        uint32_t clus = first + c;
        uint16_t link = (c + 1 < clusters) ? (uint16_t)(clus + 1) : 0xFFFF;
        put16(fat + clus * 2, link);
        /* Copy this cluster's slice of the file into the data region. */
        uint8_t *dst = sd_sector(SD_DATA_LBA + (clus - 2));
        uint32_t off = c * FAT_SECTOR;
        uint32_t n = (len - off < FAT_SECTOR) ? (len - off) : FAT_SECTOR;
        for (uint32_t i = 0; i < n; i++) dst[i] = bytes[off + i];
    }
    g_next_clus = first + clusters;

    int slot = next_root_slot();
    uint8_t *dir = sd_sector(SD_ROOT_LBA) + slot * 32;
    for (int i = 0; i < 11; i++) dir[i] = (uint8_t)name83[i];
    dir[0x0B] = 0x20;                       /* archive attribute */
    put16(dir + 0x1A, (uint16_t)first);     /* first cluster     */
    put32(dir + 0x1C, len);                 /* size              */
}

static int sd_read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    if (lba >= SD_SECTORS) return -1;
    uint8_t *src = sd_sector(lba);
    for (uint32_t i = 0; i < FAT_SECTOR; i++) buf[i] = src[i];
    return 0;
}

/* ---- graph edge rings (unused here, but pm_init wants a control plane) ---- */
static void *ring_new(void *c)                       { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)              { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;
static fat_fs_t        g_fat;

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M8 SD/FAT plugin loader (issue #34) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    /* Build the SD image: a good plugin and a wrong-ABI plugin live on the
     * "card"; the import-violating plugin is registered in the ramdisk. */
    sd_format();
    sd_add_file("PASS    ELF", (const uint8_t *)pass_elf_start,
                (uint32_t)(pass_elf_end - pass_elf_start));
    sd_add_file("BADABI  ELF", (const uint8_t *)badabi_elf_start,
                (uint32_t)(badabi_elf_end - badabi_elf_start));

    if (fat_mount(&g_fat, sd_read_block, 0) != 0) {
        uart_puts("SD: mount FAILED\r\nSD: FAIL\r\n");
        for (;;) __asm__ volatile("wfe");
    }

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_mount_sd(&g_pm, &g_fat);
    pm_register_blob(&g_pm, "rdpass", pass_elf_start,
                     (size_t)(pass_elf_end - pass_elf_start));
    pm_register_blob(&g_pm, "badimport", badimport_elf_start,
                     (size_t)(badimport_elf_end - badimport_elf_start));

    /* 1. load a plugin from the SD card. */
    long sd_pid = pm_load(&g_pm, "/sd/pass.elf");
    uart_printf("sd-load: /sd/pass.elf -> %d\r\n", (int)sd_pid);
    int sd_ok = (sd_pid > 0);
    if (sd_ok) pm_unload(&g_pm, (uint32_t)sd_pid);

    /* 2. load the same loader's other source: the ramdisk. */
    long rd_pid = pm_load(&g_pm, "/rd/rdpass");
    uart_printf("rd-load: /rd/rdpass -> %d\r\n", (int)rd_pid);
    int rd_ok = (rd_pid > 0);
    if (rd_ok) pm_unload(&g_pm, (uint32_t)rd_pid);

    /* 3. wrong ABI version rejected before any code runs. */
    long abi_r = pm_load(&g_pm, "/sd/badabi.elf");
    uart_printf("bad-abi: /sd/badabi.elf -> %d (expect %d)\r\n",
                (int)abi_r, PM_EABI);
    int abi_ok = (abi_r == PM_EABI);

    /* 4. disallowed import rejected. */
    long imp_r = pm_load(&g_pm, "badimport");
    uart_printf("bad-import: badimport -> %d (expect %d)\r\n",
                (int)imp_r, PM_EIMPORT);
    int imp_ok = (imp_r == PM_EIMPORT);

    /* 5. missing path rejected. */
    long miss_r = pm_load(&g_pm, "/sd/nope.elf");
    uart_printf("missing: /sd/nope.elf -> %d (expect %d)\r\n",
                (int)miss_r, PM_ENOENT);
    int miss_ok = (miss_r == PM_ENOENT);

    /* 6. SD load/unload is leak-free. */
    size_t base = pmm_free_pages();
    int leak_ok = 1;
    for (int i = 0; i < 20; i++) {
        long pid = pm_load(&g_pm, "/sd/pass.elf");
        if (pid <= 0) { leak_ok = 0; break; }
        if (pm_unload(&g_pm, (uint32_t)pid) != PM_OK) { leak_ok = 0; break; }
    }
    size_t after = pmm_free_pages();
    uart_printf("leak: base=%u after-20x=%u\r\n", (unsigned)base, (unsigned)after);
    leak_ok = leak_ok && (after == base);

    uart_printf("checks: sd=%d rd=%d bad-abi=%d bad-import=%d missing=%d leak=%d\r\n",
                sd_ok, rd_ok, abi_ok, imp_ok, miss_ok, leak_ok);
    uart_puts((sd_ok && rd_ok && abi_ok && imp_ok && miss_ok && leak_ok)
                  ? "SD: PASS\r\n" : "SD: FAIL\r\n");

    for (;;) __asm__ volatile("wfe");
}
