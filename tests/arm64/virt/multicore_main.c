/* tests/arm64/virt/multicore_main.c - M11 capacity + resilience on QEMU
 * 'virt' (Issue #76).
 *
 * The milestone's "done when", asserted end to end on four cores with the
 * MMU on:
 *
 *   phase 1 (capacity, single core) - six deliberately-expensive kernel
 *     nodes (each burns ~1/4 of the block budget on the cycle counter) are
 *     all assigned to one worker.  Their total cost (~1.5 blocks) cannot fit:
 *     the worker's overrun counter must climb.  The workload is real.
 *   phase 2 (capacity, distributed) - the same six nodes spread two-per-core
 *     across CPU1-3 (~0.5 blocks each) run with (QEMU-tolerance) zero
 *     overruns.  Same graph, three cores, no missed blocks.
 *   phases 3+ (resilience cycles) - real EL0 plugins on the CPU1 worker
 *     while CPU2/CPU3 keep running the expensive nodes as background load:
 *     each cycle loads the M8 `good` and `crash` plugins into isolated
 *     address spaces, runs them per block from the worker, fires `crash`
 *     mid-run (its null dereference faults at EL0 *on the worker core*, is
 *     caught by the MMU with the same fault banner as the M8 demo, and the
 *     plugin is killed), then unloads everything.  The good plugin produces
 *     sound on every block the worker runs - including the kill block - and
 *     CPU0 services every single callback.  (The watchdog check allows one
 *     QEMU-artifact overrun per kill: the fault banner's UART MMIO from CPU1
 *     takes QEMU's global lock and can stall vCPU0 for one tick; the
 *     allowance on hardware is zero.)  After all load/distribute/kill/unload
 *     cycles the frame allocator is back at its baseline: nothing leaks.
 *
 * Plumbing notes: the secondary cores join the kernel's identity map with
 * mmu_join() and install the exception vectors before entering their worker
 * loops (EL0 faults on a worker core land in that core's own handler).  The
 * kernel's EL0 resume context is single-slot (process.c), so exactly one
 * core runs EL0 code: plugin loading (CPU0) happens only between phases with
 * the timer stopped and the workers drained; per-block plugin execution
 * happens only on the CPU1 worker.
 *
 * Built MMU-on (virt_mmu.ld) with the virt GIC bases; run with -smp 4.
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
#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char good_elf_start[],  good_elf_end[];
extern char crash_elf_start[], crash_elf_end[];

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* ---- test parameters ---- */
#define FRAMES        64u               /* DAC block: 64 stereo frames      */
#define SAMPLES       (FRAMES * 2u)
#define BLOCK_HZ      1000u
#define P1_BLOCKS     300u              /* phase 1: overload one core       */
#define P2_BLOCKS     300u              /* phase 2: spread across three     */
#define CYCLES        6u                /* load/distribute/kill/unload runs */
#define CYCLE1_BLOCKS 300u
#define CYCLE_BLOCKS  60u
#define N_SPIN        6u                /* expensive nodes                  */
#define TOL_P         30u               /* 10% QEMU slack on a 300-blk phase */

/* ---- workers / audio core ---- */
static audio_worker_t g_w[3];
static spsc_ring_t    g_dac_ring;
static int16_t        g_dac_buf[4096];
static int16_t        g_dma[SAMPLES];
static audio_core_t   g_ac;

static uint64_t          g_seq;
static volatile uint32_t g_fill_on;
static volatile uint64_t g_underruns;
static uint64_t          g_spin_ticks;  /* per expensive node, per block    */

/* ---- plugin plumbing (M8 machinery) ---- */
static void *ring_new(void *c)                              { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

static plugin_t *g_good, *g_crash;
static float    *g_good_out;
static uint32_t  g_trigger;             /* worker block that fires `crash`  */
static volatile uint32_t g_cycle_blk;   /* worker-side block counter        */
static volatile uint32_t g_good_sound;  /* blocks where good produced audio */
static volatile long     g_crash_ret;   /* plugin_call_block(crash) result  */
static uint32_t          g_pattern;     /* DAC marker stream                */

static float *map_io(plugin_t *pl)
{
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
    return (float *)P2V(out_pa);
}

static long run_block(plugin_t *pl)
{
    return plugin_call_block(pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
}

static int block_has_sound(const float *out)
{
    const uint32_t *w = (const uint32_t *)out;
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        if (w[i] != 0u)
            return 1;
    return 0;
}

static void block_clear(float *out)
{
    uint32_t *w = (uint32_t *)out;      /* zero as raw words (no FP here)   */
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        w[i] = 0u;
}

/* ---- worker nodes ---- */

/* An expensive node: burn a calibrated slice of the block budget. */
static void node_spin(void *ctx)
{
    (void)ctx;
    uint64_t t0 = rd_cntpct();
    while (rd_cntpct() - t0 < g_spin_ticks)
        ;
}

/* The good plugin: one EL0 process_block per worker block; verified audio
 * becomes a counted marker block for the DAC. */
static void node_good(void *ctx)
{
    (void)ctx;
    g_cycle_blk++;
    block_clear(g_good_out);
    run_block(g_good);
    if (block_has_sound(g_good_out)) {
        g_good_sound++;
        int16_t chunk[SAMPLES];
        for (uint32_t i = 0; i < SAMPLES; i++)
            chunk[i] = (int16_t)((g_pattern + i) & 0x7FFF);
        g_pattern += SAMPLES;
        spsc_write(&g_dac_ring, chunk, SAMPLES);
    }
}

/* The crash plugin: fired once, mid-run, on this worker core.  Its null
 * dereference faults at EL0 here on CPU1 and must be caught and killed. */
static void node_crash(void *ctx)
{
    (void)ctx;
    if (g_cycle_blk == g_trigger)
        g_crash_ret = run_block(g_crash);
}

/* ---- CPU0: audio callback (timer IRQ) ---- */
static volatile uint64_t g_tick_target;   /* freeze exactly at the phase end:
                                            * a tick can land between the WFI
                                            * wake and timer_stop            */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= g_tick_target)
        return;
    g_seq++;
    for (int k = 0; k < 3; k++)
        aw_kick(&g_w[k], g_seq);

    uint64_t t0 = rd_cntpct();
    if (g_fill_on) {
        uint32_t got = audio_core_fill(&g_ac);
        if (got < SAMPLES)
            g_underruns++;
    }
    audio_wd_account(&g_ac.wd, rd_cntpct() - t0);
    g_ac.serviced++;
}

/* ---- secondary bring-up: join the MMU, take the vectors, run the loop ---- */
static void worker_entry(void *arg)
{
    mmu_join();
    exceptions_init();
    aw_worker_loop((audio_worker_t *)arg);
}

/* ---- phase plumbing (CPU0, timer stopped between phases) ---- */
static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

typedef struct { uint64_t kicks, blocks, overruns; } wsnap_t;

static void snap(wsnap_t *s)
{
    for (int k = 0; k < 3; k++) {
        s[k].kicks    = g_w[k].kicks;
        s[k].blocks   = g_w[k].blocks;
        s[k].overruns = g_w[k].overruns;
    }
}

static int drain_all(uint64_t timeout)
{
    int n = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t start = rd_cntpct();
        while (!aw_drained(&g_w[k]) && rd_cntpct() - start < timeout)
            ;
        n += aw_drained(&g_w[k]) ? 1 : 0;
    }
    return n;
}

/* Run the timer for `blocks` callbacks, then stop and drain the workers. */
static void run_blocks(uint32_t blocks, uint64_t freq)
{
    uint64_t target = g_ac.serviced + blocks;
    g_tick_target = target;
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");
    while (g_ac.serviced < target)
        __asm__ volatile("wfi");
    timer_stop();
    __asm__ volatile("msr daifset, #2");
    drain_all(freq);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M11 multi-core capacity + resilience (issue #76) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / BLOCK_HZ;
    g_spin_ticks = interval / 4u;       /* 6 nodes = 1.5 blocks of work     */

    spsc_init(&g_dac_ring, g_dac_buf, 4096);
    audio_core_init(&g_ac, &g_dac_ring, g_dma, FRAMES, interval / 2u);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "good",  good_elf_start,  (size_t)(good_elf_end  - good_elf_start));
    pm_register_blob(&g_pm, "crash", crash_elf_start, (size_t)(crash_elf_end - crash_elf_start));

    for (int k = 0; k < 3; k++)
        aw_init(&g_w[k], (uint32_t)(k + 1));
    int e1 = smp_start_core(1, worker_entry, &g_w[0],
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, worker_entry, &g_w[1],
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e3 = smp_start_core(3, worker_entry, &g_w[2],
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    uint64_t t = rd_cntpct();
    while ((!g_w[0].online || !g_w[1].online || !g_w[2].online) &&
           rd_cntpct() - t < freq)
        ;
    uint32_t online = g_w[0].online + g_w[1].online + g_w[2].online;
    uart_printf("PSCI CPU_ON: %d %d %d, workers online (MMU joined): %u/3\r\n",
                e1, e2, e3, (unsigned)online);
    gic_init();

    wsnap_t s0[3], s1[3], s2[3];

    /* ---- phase 1: the workload overruns a single core ---- */
    for (uint32_t i = 0; i < N_SPIN; i++)
        aw_assign(&g_w[0], node_spin, 0);
    snap(s0);
    run_blocks(P1_BLOCKS, freq);
    snap(s1);
    uint64_t p1_over = s1[0].overruns - s0[0].overruns;
    uart_printf("phase1 (1 core): blocks=%u overruns=%u of %u kicks\r\n",
                (unsigned)(s1[0].blocks - s0[0].blocks), (unsigned)p1_over,
                (unsigned)(s1[0].kicks - s0[0].kicks));

    /* ---- phase 2: the same workload spread across CPU1-3 ---- */
    aw_clear(&g_w[0]);
    for (int k = 0; k < 3; k++) {
        aw_assign(&g_w[k], node_spin, 0);
        aw_assign(&g_w[k], node_spin, 0);
    }
    snap(s1);
    run_blocks(P2_BLOCKS, freq);
    snap(s2);
    int p2_ok = 1;
    for (int k = 0; k < 3; k++) {
        uint64_t blk = s2[k].blocks - s1[k].blocks;
        uint64_t ovr = s2[k].overruns - s1[k].overruns;
        uart_printf("phase2 (3 cores): w%d blocks=%u overruns=%u\r\n",
                    k, (unsigned)blk, (unsigned)ovr);
        if (blk + TOL_P < P2_BLOCKS || ovr > TOL_P)
            p2_ok = 0;
    }

    /* ---- phases 3+: load / distribute / kill / unload cycles ----
     * CPU1 runs the EL0 plugins; CPU2/CPU3 keep their expensive nodes as
     * background load for the whole rest of the run. */
    aw_clear(&g_w[0]);
    size_t baseline = pmm_free_pages();

    int cycles_ok = 0, killed_all = 1;
    for (uint32_t c = 0; c < CYCLES; c++) {
        uint32_t blocks = (c == 0) ? CYCLE1_BLOCKS : CYCLE_BLOCKS;

        long gpid = pm_load(&g_pm, "good");
        long cpid = pm_load(&g_pm, "crash");
        if (gpid <= 0 || cpid <= 0)
            break;
        g_good  = pm_plugin(&g_pm, (uint32_t)gpid);
        g_crash = pm_plugin(&g_pm, (uint32_t)cpid);
        g_good_out = map_io(g_good);
        map_io(g_crash);
        if (plugin_call_init(g_good, RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK)
            break;

        g_cycle_blk  = 0;
        g_good_sound = 0;
        g_crash_ret  = 1000;            /* sentinel: not fired yet          */
        g_trigger    = blocks / 2u;
        g_dac_ring.head = g_dac_ring.tail = 0;
        int16_t zero[SAMPLES];
        for (uint32_t i = 0; i < SAMPLES; i++)
            zero[i] = 0;
        spsc_write(&g_dac_ring, zero, SAMPLES);
        spsc_write(&g_dac_ring, zero, SAMPLES);

        aw_assign(&g_w[0], node_good, 0);
        aw_assign(&g_w[0], node_crash, 0);

        wsnap_t a[3], b[3];
        snap(a);
        uint64_t under0 = g_underruns;
        g_fill_on = 1;
        run_blocks(blocks, freq);
        g_fill_on = 0;
        snap(b);

        aw_clear(&g_w[0]);
        pm_unload(&g_pm, (uint32_t)gpid);
        pm_unload(&g_pm, (uint32_t)cpid);

        uint64_t wblk  = b[0].blocks - a[0].blocks;
        uint64_t wovr  = b[0].overruns - a[0].overruns;
        uint64_t lblk1 = b[1].blocks - a[1].blocks;
        uint64_t lblk2 = b[2].blocks - a[2].blocks;
        uint64_t under = g_underruns - under0;
        /* TCG timing bounds only - the exact invariants (kill contained,
         * sound on every executed block, zero leak, every callback serviced)
         * are asserted exactly.  The EL0 round trips on CPU1 cost all vCPUs
         * translation-cache and scheduling time under TCG (worst in cycle 1,
         * which translates the plugin code fresh), so the executed-block
         * bounds carry ~15-20% slack; on hardware they would be exact. */
        uint32_t tol   = (blocks * 3u) / 20u + 4u;
        uint32_t tol_l = blocks / 5u + 4u;
        int killed     = (g_crash_ret == -1);
        int sound_ok   = (g_good_sound == wblk);
        int alive      = (lblk1 + tol_l >= blocks) && (lblk2 + tol_l >= blocks);
        int ok = killed && sound_ok && (wblk + tol >= blocks) &&
                 (wovr <= tol) && (under <= tol) && alive;
        killed_all &= killed;
        cycles_ok  += ok;
        uart_printf("  cycle %u: killed=%d good-sound=%u/%u load=%u+%u/%u underruns=%u = %s\r\n",
                    (unsigned)(c + 1), killed, (unsigned)g_good_sound,
                    (unsigned)wblk, (unsigned)lblk1, (unsigned)lblk2,
                    (unsigned)blocks, (unsigned)under, ok ? "yes" : "NO");
    }

    size_t after = pmm_free_pages();
    int no_leak = (after == baseline);
    int drained = drain_all(freq);
    for (int k = 0; k < 3; k++)
        aw_stop(&g_w[k]);

    uart_printf("leak: baseline=%u after-%ux=%u no-leak=%d\r\n",
                (unsigned)baseline, (unsigned)CYCLES, (unsigned)after, no_leak);
    uart_printf("audio: serviced=%u overruns=%u worst=%u cyc (budget=%u)\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_ac.wd.overruns,
                (unsigned)g_ac.wd.worst, (unsigned)g_ac.wd.budget);

    int all_online = (online == 3) && (e1 == 0) && (e2 == 0) && (e3 == 0);
    int p1_ok      = (p1_over >= P1_BLOCKS / 5u);   /* the workload is real */
    /* CPU0 must service every callback.  The overrun allowance is a QEMU
     * artifact, not slack in the contract: each kill prints the multi-line
     * fault banner from CPU1's handler, and every UART MMIO write takes
     * QEMU's global lock, which can stall vCPU0 mid-fill for one tick.  On
     * hardware CPU1's UART writes do not stall CPU0; the allowance is zero. */
    uint64_t total = P1_BLOCKS + P2_BLOCKS + CYCLE1_BLOCKS +
                     (CYCLES - 1u) * CYCLE_BLOCKS;
    int cpu0_ok    = (g_ac.serviced == total) && (g_ac.wd.overruns <= CYCLES);

    uart_printf("checks: online=%d phase1-overruns=%d phase2-clean=%d cycles=%d/%u cpu0-clean=%d no-leak=%d drained=%d/3\r\n",
                all_online, p1_ok, p2_ok, cycles_ok, (unsigned)CYCLES,
                cpu0_ok, no_leak, drained);

    int ok = all_online && p1_ok && p2_ok && (cycles_ok == (int)CYCLES) &&
             killed_all && cpu0_ok && no_leak && (drained == 3);
    uart_puts(ok ? "MULTICORE: PASS\r\n" : "MULTICORE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
