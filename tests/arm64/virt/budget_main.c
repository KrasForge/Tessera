/* tests/arm64/virt/budget_main.c - CPU-budget enforcement on QEMU 'virt'
 * (Issue #78).
 *
 * The time-safety leg of the sandbox, end to end with the MMU on:
 *
 *   CPU0 - the audio core: kicks the worker each 1 kHz block, drains the DAC
 *          ring, never blocks, never prints during the run.
 *   CPU1 - the plugin worker (MMU joined, own vectors, own GIC interface and
 *          banked timer PPI).  Three EL0 plugins run per block, each wrapped
 *          in budget enforcement:
 *            good - renders audio; never trips its budget.
 *            blip - spins forever on its 3rd and 4th blocks, then behaves.
 *            hog  - spins forever in every process_block.
 *
 * Every budgeted call arms the worker core's generic timer for the plugin's
 * budget and opens the EL0 IRQ window; a plugin still running at the budget
 * boundary is preempted mid-block by the timer IRQ - never at the block
 * boundary: every preempted run measures between the budget and the block
 * period - and the escalation policy applies.  hog collects 3 consecutive
 * offences (mute, mute, kill) and is removed; blip collects 2, is muted
 * twice, is forgiven on its clean block, and is audibly back; good never
 * offends.  Throughout, CPU0 services every callback and the DAC never
 * starves beyond the usual QEMU tolerance.  (The watchdog allowance of one
 * overrun per [budget] banner is the same QEMU UART/BQL artifact as in the
 * multicore harness - each banner is UART MMIO from CPU1 under QEMU's
 * global lock; the allowance on hardware is zero.)  Unloading everything
 * returns the frame allocator to its baseline: a budget kill leaks nothing.
 *
 * Budgets come from the control plane (gc_set_budget); a plugin with none
 * set gets the fair share of the block across the worker's nodes.
 *
 * Built MMU-on (virt_mmu.ld) with the virt GIC bases; run with -smp 2.
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
#include "budget.h"
#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
#include "gic.h"
#include "timer.h"
#include "latency.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char good_elf_start[], good_elf_end[];
extern char blip_elf_start[], blip_elf_end[];
extern char hog_elf_start[],  hog_elf_end[];

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* ---- test parameters ---- */
#define FRAMES       64u
#define SAMPLES      (FRAMES * 2u)
#define BLOCK_HZ     1000u
#define BLOCKS       100u
#define N_NODES      3u
#define KILL_AFTER   3u
#define TOL          15u               /* worker-lateness slack (TCG)       */
#define WD_ALLOW     8u                /* one per [budget] banner (QEMU BQL) */

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
    uint32_t *w = (uint32_t *)out;
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        w[i] = 0u;
}

/* ---- the budgeted node wrapper (the host's enforcement point) ---- */
static audio_worker_t g_w;
static spsc_ring_t    g_dac_ring;
static int16_t        g_dac_buf[4096];
static int16_t        g_dma[SAMPLES];
static audio_core_t   g_ac;

typedef struct {
    const char *name;
    plugin_t   *pl;
    float      *out;
    budget_t    pol;
    int         slot;             /* aw node slot, for the stats line      */
    int         to_dac;           /* good: deliver a marker into the DAC   */
    volatile uint32_t runs;       /* budgeted attempts (incl. preempted)   */
    volatile uint32_t muted;      /* blocks silenced by policy             */
    volatile uint32_t audible;    /* blocks that produced verified sound   */
    volatile uint32_t kill_logged;
    uint64_t    preempt_min;      /* shortest preempted run (cycles)       */
    uint64_t    preempt_max;      /* longest preempted run (cycles)        */
} bnode_t;

static bnode_t  g_good, g_blip, g_hog;
static uint32_t g_pattern;
static uint64_t g_freq;

static void node_budgeted(void *ctx)
{
    bnode_t *n = ctx;
    if (n->pol.killed)
        return;                        /* removed from service             */

    n->runs++;
    block_clear(n->out);

    uint64_t t0 = rd_cntpct();
    budget_arm(n->pol.cycles);
    long r = run_block(n->pl);
    budget_disarm();
    uint64_t dt = rd_cntpct() - t0;

    int over = (r == BUDGET_PREEMPTED);
    if (over) {
        if (n->preempt_min == 0 || dt < n->preempt_min)
            n->preempt_min = dt;
        if (dt > n->preempt_max)
            n->preempt_max = dt;
    }

    int act = budget_account(&n->pol, over);
    g_w.nodes[n->slot].offences = n->pol.offences;   /* stats line (#77)  */

    if (act == BUDGET_KILL) {
        if (!n->kill_logged) {
            n->kill_logged = 1;
            uart_printf("  [budget] kill pid=%u (%s) after %u consecutive offences (last=%uus budget=%uus)\r\n",
                        (unsigned)n->pl->proc->pid, n->name,
                        (unsigned)n->pol.streak,
                        (unsigned)lat_cyc_to_us(dt, g_freq),
                        (unsigned)lat_cyc_to_us(n->pol.cycles, g_freq));
        }
        n->muted++;
        return;                        /* output stays silent              */
    }
    if (act == BUDGET_MUTE) {
        n->muted++;                    /* offence: silence downstream      */
        return;
    }

    if (block_has_sound(n->out)) {
        n->audible++;
        if (n->to_dac) {               /* verified audio -> the DAC ring   */
            int16_t chunk[SAMPLES];
            for (uint32_t i = 0; i < SAMPLES; i++)
                chunk[i] = (int16_t)((g_pattern + i) & 0x7FFF);
            g_pattern += SAMPLES;
            spsc_write(&g_dac_ring, chunk, SAMPLES);
        }
    }
}

/* ---- CPU0: audio callback (timer IRQ) ---- */
static volatile uint64_t g_tick_target;
static volatile uint64_t g_underruns;
static uint64_t          g_seq;

void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= g_tick_target)
        return;
    g_seq++;
    aw_kick(&g_w, g_seq);

    uint64_t t0 = rd_cntpct();
    uint32_t got = audio_core_fill(&g_ac);
    if (got < SAMPLES)
        g_underruns++;
    audio_wd_account(&g_ac.wd, rd_cntpct() - t0);
    g_ac.serviced++;
}

/* ---- CPU1 bring-up: MMU, vectors, GIC interface, budget PPI ---- */
static uint8_t g_stack1[16384] __attribute__((aligned(16)));

static void worker_entry(void *arg)
{
    mmu_join();
    exceptions_init();
    gic_cpu_init();                    /* banked CPU interface             */
    gic_enable_irq(TIMER_IRQ);         /* banked PPI: this core's timer    */
    aw_worker_loop((audio_worker_t *)arg);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt CPU-budget enforcement (issue #78) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(g_freq));
    uint64_t interval = g_freq / BLOCK_HZ;

    spsc_init(&g_dac_ring, g_dac_buf, 4096);
    audio_core_init(&g_ac, &g_dac_ring, g_dma, FRAMES, interval / 2u);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "good", good_elf_start, (size_t)(good_elf_end - good_elf_start));
    pm_register_blob(&g_pm, "blip", blip_elf_start, (size_t)(blip_elf_end - blip_elf_start));
    pm_register_blob(&g_pm, "hog",  hog_elf_start,  (size_t)(hog_elf_end  - hog_elf_start));

    size_t baseline = pmm_free_pages();

    long gpid = pm_load(&g_pm, "good");
    long bpid = pm_load(&g_pm, "blip");
    long hpid = pm_load(&g_pm, "hog");
    if (gpid <= 0 || bpid <= 0 || hpid <= 0) {
        uart_puts("plugin load failed\r\nBUDGET: FAIL\r\n");
        for (;;) __asm__ volatile("wfe");
    }
    /* pm_load registered each pid with the graph, so budgets can be set. */
    uint64_t hard_budget = interval / 3u;              /* ~333 us          */
    int sb = gc_set_budget(&g_gc, (uint32_t)bpid, hard_budget);
    int sh = gc_set_budget(&g_gc, (uint32_t)hpid, hard_budget);

    g_good.name = "good"; g_good.pl = pm_plugin(&g_pm, (uint32_t)gpid);
    g_blip.name = "blip"; g_blip.pl = pm_plugin(&g_pm, (uint32_t)bpid);
    g_hog.name  = "hog";  g_hog.pl  = pm_plugin(&g_pm, (uint32_t)hpid);
    g_good.out = map_io(g_good.pl);
    g_blip.out = map_io(g_blip.pl);
    g_hog.out  = map_io(g_hog.pl);
    g_good.to_dac = 1;

    if (plugin_call_init(g_good.pl, RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK ||
        plugin_call_init(g_blip.pl, RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK ||
        plugin_call_init(g_hog.pl,  RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK) {
        uart_puts("plugin init failed\r\nBUDGET: FAIL\r\n");
        for (;;) __asm__ volatile("wfe");
    }

    /* Budgets: control-plane values, or the fair share when none is set. */
    uint64_t fair = budget_fair_share(interval, N_NODES);
    uint64_t bg = gc_budget(&g_gc, (uint32_t)gpid);
    uint64_t bb = gc_budget(&g_gc, (uint32_t)bpid);
    uint64_t bh = gc_budget(&g_gc, (uint32_t)hpid);
    budget_init(&g_good.pol, bg ? bg : fair, KILL_AFTER);
    budget_init(&g_blip.pol, bb ? bb : fair, KILL_AFTER);
    budget_init(&g_hog.pol,  bh ? bh : fair, KILL_AFTER);

    /* One worker on CPU1; nodes in fixed order, tagged for the stats line. */
    aw_init(&g_w, 1);
    g_w.clock = rd_cntpct;             /* issue #77 accounting stays on    */
    g_good.slot = aw_assign(&g_w, node_budgeted, &g_good);
    g_blip.slot = aw_assign(&g_w, node_budgeted, &g_blip);
    g_hog.slot  = aw_assign(&g_w, node_budgeted, &g_hog);
    g_w.nodes[g_good.slot].tag = (uint32_t)gpid;
    g_w.nodes[g_blip.slot].tag = (uint32_t)bpid;
    g_w.nodes[g_hog.slot].tag  = (uint32_t)hpid;

    /* Prime the DAC ring (one-block pipeline + TCG slack). */
    int16_t zero[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++)
        zero[i] = 0;
    spsc_write(&g_dac_ring, zero, SAMPLES);
    spsc_write(&g_dac_ring, zero, SAMPLES);

    gic_init();                        /* distributor up before CPU1 joins */
    int e1 = smp_start_core(1, worker_entry, &g_w,
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    uint64_t t = rd_cntpct();
    while (!g_w.online && rd_cntpct() - t < g_freq)
        ;
    uart_printf("PSCI CPU_ON: worker=%d online=%u budgets: set=%d,%d fair=%uus hard=%uus\r\n",
                e1, (unsigned)g_w.online, sb, sh,
                (unsigned)lat_cyc_to_us(fair, g_freq),
                (unsigned)lat_cyc_to_us(hard_budget, g_freq));

    g_tick_target = BLOCKS;
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    while (g_ac.serviced < BLOCKS)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    uint64_t start = rd_cntpct();
    while (!aw_drained(&g_w) && rd_cntpct() - start < g_freq)
        ;
    int drained = aw_drained(&g_w);
    aw_stop(&g_w);

    /* Unload everything - including the budget-killed hog: leak-free. */
    pm_unload(&g_pm, (uint32_t)gpid);
    pm_unload(&g_pm, (uint32_t)bpid);
    pm_unload(&g_pm, (uint32_t)hpid);
    size_t after = pmm_free_pages();
    int no_leak = (after == baseline);

    /* ---- report ---- */
    uart_printf("audio: serviced=%u underruns=%u overruns=%u worst=%u cyc\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns, (unsigned)g_ac.wd.worst);
    uart_printf("worker: kicks=%u blocks=%u overruns=%u\r\n",
                (unsigned)g_w.kicks, (unsigned)g_w.blocks,
                (unsigned)g_w.overruns);
    uart_printf("good: runs=%u audible=%u offences=%u muted=%u killed=%u\r\n",
                (unsigned)g_good.runs, (unsigned)g_good.audible,
                (unsigned)g_good.pol.offences, (unsigned)g_good.muted,
                (unsigned)g_good.pol.killed);
    uart_printf("blip: runs=%u audible=%u offences=%u muted=%u killed=%u preempt=[%u,%u]us\r\n",
                (unsigned)g_blip.runs, (unsigned)g_blip.audible,
                (unsigned)g_blip.pol.offences, (unsigned)g_blip.muted,
                (unsigned)g_blip.pol.killed,
                (unsigned)lat_cyc_to_us(g_blip.preempt_min, g_freq),
                (unsigned)lat_cyc_to_us(g_blip.preempt_max, g_freq));
    uart_printf("hog: runs=%u offences=%u muted=%u killed=%u preempt=[%u,%u]us\r\n",
                (unsigned)g_hog.runs, (unsigned)g_hog.pol.offences,
                (unsigned)g_hog.muted, (unsigned)g_hog.pol.killed,
                (unsigned)lat_cyc_to_us(g_hog.preempt_min, g_freq),
                (unsigned)lat_cyc_to_us(g_hog.preempt_max, g_freq));
    uart_printf("leak: baseline=%u after=%u no-leak=%d\r\n",
                (unsigned)baseline, (unsigned)after, no_leak);

    /* ---- checks ---- */
    int online   = (e1 == 0) && g_w.online;
    int ctl_ok   = (sb == GC_OK) && (sh == GC_OK) &&
                   (g_blip.pol.cycles == hard_budget) &&
                   (g_good.pol.cycles == fair);
    int cpu0_ok  = (g_ac.serviced == BLOCKS) &&
                   (g_ac.wd.overruns <= WD_ALLOW) && (g_underruns <= TOL);
    int w_ok     = (g_w.blocks + g_w.overruns == g_w.kicks) &&
                   (g_w.overruns <= TOL) && drained;
    /* hog: three consecutive offences - mute, mute, kill - then removed. */
    int hog_ok   = g_hog.pol.killed && (g_hog.pol.offences == KILL_AFTER) &&
                   (g_hog.runs == KILL_AFTER) && (g_hog.muted == KILL_AFTER) &&
                   g_hog.kill_logged;
    /* blip: two offences, forgiven, audibly back, never killed. */
    int blip_ok  = !g_blip.pol.killed && (g_blip.pol.offences == 2) &&
                   (g_blip.muted == 2) && (g_blip.runs >= 5) &&
                   (g_blip.audible == g_blip.runs - 2);
    /* good: audio every run, zero offences - well-behaved plugins see no
     * behaviour change. */
    int good_ok  = (g_good.pol.offences == 0) && (g_good.muted == 0) &&
                   (g_good.audible == g_good.runs) && (g_good.runs > 0);
    /* Preemption happened at the budget boundary, mid-block: every
     * preempted run measures at or above the budget and clearly below the
     * block period. */
    int preempt_ok = (g_hog.preempt_min >= hard_budget) &&
                     (g_hog.preempt_min < interval) &&
                     (g_blip.preempt_min >= hard_budget) &&
                     (g_blip.preempt_min < interval);

    uart_printf("checks: online=%d ctl=%d cpu0=%d worker=%d hog=%d blip=%d good=%d preempt=%d no-leak=%d\r\n",
                online, ctl_ok, cpu0_ok, w_ok, hog_ok, blip_ok, good_ok,
                preempt_ok, no_leak);

    int ok = online && ctl_ok && cpu0_ok && w_ok && hog_ok && blip_ok &&
             good_ok && preempt_ok && no_leak;
    uart_puts(ok ? "BUDGET: PASS\r\n" : "BUDGET: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
