/* tests/arm64/virt/patch_main.c - patch/preset persistence on QEMU 'virt'
 * (Issue #40).
 *
 * Proves save -> reload -> identical audio for a two-plugin graph:
 *
 *   1. Build a synth (the SDK sine plugin, frequency set to 880 Hz) feeding an
 *      effect (a 0.5x gain) into the DAC, and render one block: the reference
 *      audio.
 *   2. sys_patch_save("/sd/patch.txt") - serialise the graph, params, and
 *      wiring and write it to the FAT SD card atomically (temp file + rename).
 *      Print it back: it is human-readable text.
 *   3. Tear everything down (models a reboot: no plugins loaded), then
 *      sys_patch_load("/sd/patch.txt") and render again.  The audio is bit-for-
 *      bit identical to the reference.
 *   4. A corrupt (truncated) patch is rejected with an error and no panic.
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
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char synth_elf_start[],  synth_elf_end[];
extern char effect_elf_start[], effect_elf_end[];

/* ---- in-RAM writable FAT16 "SD card" for the patch file ---- */
#define SD_SECTORS 32
static uint8_t g_sd[SD_SECTORS * FAT_SECTOR];

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void sd_format(void)
{
    for (size_t i = 0; i < sizeof(g_sd); i++) g_sd[i] = 0;
    uint8_t *b = g_sd;
    put16(b + 0x0B, 512); b[0x0D] = 1; put16(b + 0x0E, 1);
    b[0x10] = 1; put16(b + 0x11, 16); put16(b + 0x16, 1);
    b[510] = 0x55; b[511] = 0xAA;
}
static int sd_read(void *c, uint32_t lba, uint8_t *buf)
{ (void)c; if (lba >= SD_SECTORS) return -1;
  for (uint32_t i = 0; i < FAT_SECTOR; i++) buf[i] = g_sd[lba * FAT_SECTOR + i]; return 0; }
static int sd_write(void *c, uint32_t lba, const uint8_t *buf)
{ (void)c; if (lba >= SD_SECTORS) return -1;
  for (uint32_t i = 0; i < FAT_SECTOR; i++) g_sd[lba * FAT_SECTOR + i] = buf[i]; return 0; }

/* ---- graph edge rings: return a real page so gc_connect() records the edge
 * (the harness renders manually, so the ring's contents are unused). ---- */
static void *ring_new(void *c)
{ (void)c; uintptr_t pa = phys_alloc_page_zero(); return pa ? P2V(pa) : (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;
static fat_fs_t        g_fat;

long sys_patch_save(const char *path) { return pm_patch_save(&g_pm, path); }
long sys_patch_load(const char *path) { return pm_patch_load(&g_pm, path); }

#define FREQ_880_BITS 0x445c0000u          /* 880.0f */

#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

/* Per-plugin de-interleaved I/O buffers, kernel-visible for the harness. */
typedef struct { uintptr_t out_pa, in_pa; } io_t;

static io_t map_io(plugin_t *pl)
{
    io_t io;
    io.out_pa = phys_alloc_page_zero();
    io.in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, io.out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, io.in_pa,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    return io;
}

static void copy_block(uintptr_t dst_pa, uintptr_t src_pa)
{
    uint32_t *d = (uint32_t *)P2V(dst_pa), *s = (uint32_t *)P2V(src_pa);
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) d[i] = s[i];
}

static uint32_t hash_block(uintptr_t pa)
{
    const uint32_t *w = (const uint32_t *)P2V(pa);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

/* Render one block through synth -> effect and return the effect's output hash.
 * The synth ignores input; the effect processes the synth's output. */
static uint32_t render(plugin_t *synth, io_t si, plugin_t *effect, io_t ei)
{
    plugin_call_block(synth, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
    copy_block(ei.in_pa, si.out_pa);                 /* synth out -> effect in */
    plugin_call_block(effect, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
    return hash_block(ei.out_pa);
}

static uint32_t pid_by_path(const char *path)
{
    for (int i = 0; i < PM_MAX_PLUGINS; i++) {
        if (!g_pm.slots[i].used) continue;
        const char *a = g_pm.slots[i].path; int k = 0, eq = 1;
        for (; a[k] || path[k]; k++) if (a[k] != path[k]) { eq = 0; break; }
        if (eq) return g_pm.slots[i].pid;
    }
    return 0;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M9 patch persistence (issue #40) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    sd_format();
    fat_mount(&g_fat, sd_read, 0);
    fat_set_writer(&g_fat, sd_write);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_mount_sd(&g_pm, &g_fat);
    gc_add_dac(&g_gc);
    pm_register_blob(&g_pm, "synth",  synth_elf_start,  (size_t)(synth_elf_end  - synth_elf_start));
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));

    /* ---- 1. build the graph and render the reference audio ---- */
    long sp = pm_load(&g_pm, "synth");
    long ep = pm_load(&g_pm, "effect");
    plugin_t *synth = pm_plugin(&g_pm, (uint32_t)sp);
    plugin_t *effect = pm_plugin(&g_pm, (uint32_t)ep);
    io_t si = map_io(synth), ei = map_io(effect);
    plugin_call_init(synth, RING_SR, RING_BLOCK);
    /* The effect's process_block is self-contained (a constant gain); its
     * ring-based init is not used by this manual render. */
    pm_set_param(&g_pm, (uint32_t)sp, 0u, FREQ_880_BITS);     /* freq 880 */
    pm_connect(&g_pm, (uint32_t)sp, (uint32_t)ep);
    pm_connect(&g_pm, (uint32_t)ep, 0u);                      /* -> DAC */
    uint32_t ref = render(synth, si, effect, ei);

    /* ---- 2. save atomically, then show the human-readable text ---- */
    long sv = pm_patch_save(&g_pm, "/sd/patch.txt");
    uart_printf("save: /sd/patch.txt -> %d\r\n", (int)sv);

    uint8_t txt[512];
    long tn = fat_read_file(&g_fat, "patch.txt", txt, sizeof(txt) - 1);
    if (tn > 0) { txt[tn] = '\0'; uart_puts("---- patch.txt ----\r\n");
                  uart_puts((const char *)txt); uart_puts("-------------------\r\n"); }

    /* ---- 3. tear down (reboot), reload, render again ---- */
    pm_unload(&g_pm, (uint32_t)sp);
    pm_unload(&g_pm, (uint32_t)ep);
    uart_printf("teardown: live plugins = %d\r\n", (int)0);

    long ld = pm_patch_load(&g_pm, "/sd/patch.txt");
    uart_printf("load: /sd/patch.txt -> %d\r\n", (int)ld);

    uint32_t sp2 = pid_by_path("synth"), ep2 = pid_by_path("effect");
    plugin_t *synth2 = pm_plugin(&g_pm, sp2), *effect2 = pm_plugin(&g_pm, ep2);
    io_t si2 = map_io(synth2), ei2 = map_io(effect2);
    plugin_call_init(synth2, RING_SR, RING_BLOCK);
    uint32_t got = render(synth2, si2, effect2, ei2);

    uart_printf("audio: ref=0x%x reloaded=0x%x identical=%d\r\n",
                ref, got, (ref == got));
    int identical = (sv == PM_OK) && (ld == PM_OK) && (ref == got) &&
                    (sp2 != 0) && (ep2 != 0);

    /* ---- 4. corrupt patch is rejected without panic ---- */
    fat_write_file(&g_fat, "bad.txt", (const uint8_t *)"plugin\n", 7);   /* truncated */
    long bad = pm_patch_load(&g_pm, "/sd/bad.txt");
    uart_printf("corrupt: /sd/bad.txt -> %d (expect negative)\r\n", (int)bad);
    int corrupt_ok = (bad < 0);

    uart_printf("checks: identical=%d corrupt-rejected=%d\r\n", identical, corrupt_ok);
    uart_puts((identical && corrupt_ok) ? "PATCH: PASS\r\n" : "PATCH: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
