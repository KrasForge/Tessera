/* tests/arm64/virt/hot_reload_main.c - plugin hot-reload without a dropout on
 * QEMU 'virt' (Theme A: reliability).
 *
 * Replace a running plugin's ELF live, with no gap in the audio.  The harness
 * drives the real plugin manager through the reload state machine:
 *
 *   1. load the plugin (generation 0) into its own isolated address space and
 *      play blocks through it;
 *   2. begin a reload - load the new version (generation 1) into a *second*,
 *      fresh address space while generation 0 keeps producing (both instances
 *      coexist, isolated, at once);
 *   3. at a block boundary, swap production to generation 1 and retire
 *      generation 0, freeing its address space;
 *   4. keep playing through generation 1.
 *
 * It asserts the deterministic, product-relevant facts:
 *   - the DAC is never silent across the swap - no dropped block;
 *   - the two generations really are separate isolated processes (distinct
 *     pids, both live at the moment of overlap);
 *   - the swap commits exactly once and the old generation is genuinely retired
 *     (its pid no longer resolves);
 *   - unloading everything returns the frame allocator to baseline (the retired
 *     address space was fully reclaimed - no leak).
 *
 * Built MMU-on (virt_mmu.ld), single core.  FP-free: the plugin runs its DSP at
 * EL0; the harness feeds a fixed tone as raw float bit-patterns and moves plugin
 * pages as raw words.
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
#include "ring_contract.h"
#include "hot_reload.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char effect_elf_start[], effect_elf_end[];

static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

static void *ring_new(void *c)                              { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

#define WORDS     (RING_BLOCK * 2u)
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

#define PRE   3u          /* blocks on gen 0 before the reload */
#define POST  3u          /* blocks on gen 1 after the swap    */

/* A fixed, strictly-positive tone as raw float bits (0.5, 0.3, 0.1, 0.3): the
 * DC component guarantees the low-pass output is never silent, the variation
 * makes it a real signal.  No FP used here - the plugin does the DSP at EL0. */
static const uint32_t TONE[4] = { 0x3F000000u, 0x3E99999Au, 0x3DCCCCCDu, 0x3E99999Au };

typedef struct { uint32_t *in_k; uint32_t *out_k; } io_t;

static io_t map_io(plugin_t *pl)
{
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
    io_t io = { (uint32_t *)P2V(in_pa), (uint32_t *)P2V(out_pa) };
    for (uint32_t i = 0; i < RING_BLOCK; i++) {
        io.in_k[i]              = TONE[i & 3u];       /* L */
        io.in_k[RING_BLOCK + i] = TONE[i & 3u];       /* R */
    }
    return io;
}

static long run_block(plugin_t *pl)
{
    return plugin_call_block(pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
}
static int has_sound(const uint32_t *w)
{
    for (uint32_t i = 0; i < WORDS; i++) if (w[i]) return 1;
    return 0;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt plugin hot-reload (Theme A: reliability) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));

    size_t baseline = pmm_free_pages();

    hr_state_t hr;
    hr_init(&hr);

    uint32_t pid_of[2] = { 0u, 0u };
    io_t     io_of[2];
    uint32_t sound = 0, total = 0;
    uint32_t retired;

    /* Generation 0: load, map, init, and play. */
    long p0 = pm_load(&g_pm, "effect");
    pid_of[0] = (uint32_t)(p0 > 0 ? p0 : 0);
    if (p0 > 0) {
        plugin_t *g = pm_plugin(&g_pm, pid_of[0]);
        io_of[0] = map_io(g);
        plugin_call_init(g, RING_SR, RING_BLOCK);
    }

    for (uint32_t b = 0; b < PRE; b++) {
        uint32_t gen = hr_next(&hr, &retired);        /* steady: gen 0 */
        run_block(pm_plugin(&g_pm, pid_of[gen]));
        if (has_sound(io_of[gen].out_k)) sound++;
        total++;
    }

    /* Begin the reload: load generation 1 into a fresh isolated space while
     * generation 0 is still loaded and running. */
    int prepared = hr_prepare(&hr);
    long p1 = pm_load(&g_pm, "effect");
    pid_of[1] = (uint32_t)(p1 > 0 ? p1 : 0);
    int distinct   = (p1 > 0) && (pid_of[1] != pid_of[0]);
    int both_live  = (pm_plugin(&g_pm, pid_of[0]) != 0) && (pm_plugin(&g_pm, pid_of[1]) != 0);
    if (p1 > 0) {
        plugin_t *g = pm_plugin(&g_pm, pid_of[1]);
        io_of[1] = map_io(g);
        plugin_call_init(g, RING_SR, RING_BLOCK);
    }
    hr_ready(&hr, p1 > 0);

    /* The swap boundary: gen 1 takes over, gen 0 is retired and unloaded. */
    uint32_t gen = hr_next(&hr, &retired);
    int swapped_to_new = (gen == 1u);
    int retired_old    = (retired == 0u);
    if (retired != HR_NONE)
        pm_unload(&g_pm, pid_of[retired]);
    int old_gone   = (pm_plugin(&g_pm, pid_of[0]) == 0);
    int new_live   = (pm_plugin(&g_pm, pid_of[1]) != 0);
    run_block(pm_plugin(&g_pm, pid_of[gen]));
    if (has_sound(io_of[gen].out_k)) sound++;
    total++;

    /* Keep playing through generation 1. */
    for (uint32_t b = 1; b < POST; b++) {
        gen = hr_next(&hr, &retired);                 /* steady: gen 1 */
        run_block(pm_plugin(&g_pm, pid_of[gen]));
        if (has_sound(io_of[gen].out_k)) sound++;
        total++;
    }

    pm_unload(&g_pm, pid_of[1]);
    size_t final = pmm_free_pages();
    int no_leak = (final == baseline);

    uart_printf("gen0-pid=%u gen1-pid=%u distinct=%d both-live=%d prepared=%d\r\n",
                (unsigned)pid_of[0], (unsigned)pid_of[1], distinct, both_live, prepared);
    uart_printf("swap: to-new=%d retired-old=%d old-gone=%d new-live=%d swaps=%u\r\n",
                swapped_to_new, retired_old, old_gone, new_live, (unsigned)hr.swaps);
    uart_printf("never-silent=%u/%u  leak: baseline=%u after=%u no-leak=%d\r\n",
                (unsigned)sound, (unsigned)total,
                (unsigned)baseline, (unsigned)final, no_leak);

    int ok = (pid_of[0] > 0u) && distinct && both_live && prepared &&
             swapped_to_new && retired_old && old_gone && new_live &&
             (hr.swaps == 1u) && (sound == total) && no_leak;

    uart_puts(ok ? "HOT-RELOAD: PASS\r\n" : "HOT-RELOAD: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
