/* tests/arm64/virt/resilience_main.c - resilience demo on QEMU 'virt'
 * (Issue #36, M8; extended with the time-safety leg by issue #79, M12).
 *
 * The M8 "done when": an externally-supplied plugin binary is loaded at
 * runtime, sandboxed, and crashing it does not disturb the audio engine or
 * other plugins.  The M12 "done when" adds the plugin the MMU cannot catch.
 * This harness is that demo, made deterministic:
 *
 *   - Four plugins are loaded into isolated, sandboxed address spaces and
 *     wired into a graph: good (a clean 440 Hz sine), crash
 *     (null-dereferences in process_block), evil (issues an SVC from the
 *     audio path, then a wild kernel write), and hog (spins forever in
 *     process_block - no bad access, no syscall, just stolen time).
 *   - Every block, the good plugin's process_block runs and the "DAC" (this
 *     host) reads real audio from its output.  At the trigger block
 *     (modelling "after 3 seconds") the crash and evil plugins are run and
 *     are each caught and killed, and the hog starts running under its CPU
 *     budget: it is preempted at its budget boundary every block, muted on
 *     its first offences, and killed by the escalation policy on the third
 *     consecutive one - the good plugin and the DAC never miss a block.
 *   - The whole load / run / kill / unload cycle repeats 10 times; the frame
 *     allocator's free count returns exactly to baseline, so nothing leaks -
 *     including the budget-killed hog.
 *
 * All four containment mechanisms are exercised and logged: the MMU data
 * abort (crash's null dereference), the wild kernel write (evil's second
 * act, same abort path), the syscall gate (evil's SVC from process_block),
 * and the budget kill (hog, issue #78 - the sandbox's time leg).
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
#include "gic.h"
#include "timer.h"
#include "latency.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char good_elf_start[],  good_elf_end[];
extern char crash_elf_start[], crash_elf_end[];
extern char evil_elf_start[],  evil_elf_end[];
extern char hog_elf_start[],   hog_elf_end[];

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t g_freq;        /* CNTFRQ_EL0                                */
static uint64_t g_hog_budget;  /* fair share of one audio block, 4 plugins  */

/* Graph control plane (pm_init wants one); edge rings are unused here. */
static void *ring_new(void *c)                              { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

#define TOTAL_BLOCKS  6u
#define TRIGGER_BLOCK 3u          /* fire the hostile plugins here ("3 s") */

/* Give a loaded plugin its de-interleaved input/output buffers and return the
 * kernel-visible pointer to its output page (for the DAC to read). */
static float *map_io(plugin_t *pl)
{
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
    return (float *)P2V(out_pa);
}

/* out_l occupies [0, RING_BLOCK) floats of the output page; out_r follows it. */
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

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
    uint32_t *w = (uint32_t *)out;      /* zero as raw words (no FP here) */
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        w[i] = 0u;
}

/* One demo pass: returns 1 if the graph behaved (good sounded every block,
 * both hostile plugins were killed at the trigger). */
static int demo_pass(void)
{
    long gpid = pm_load(&g_pm, "good");
    long cpid = pm_load(&g_pm, "crash");
    long epid = pm_load(&g_pm, "evil");
    long hpid = pm_load(&g_pm, "hog");
    if (gpid <= 0 || cpid <= 0 || epid <= 0 || hpid <= 0)
        return 0;

    plugin_t *good  = pm_plugin(&g_pm, (uint32_t)gpid);
    plugin_t *crash = pm_plugin(&g_pm, (uint32_t)cpid);
    plugin_t *evil  = pm_plugin(&g_pm, (uint32_t)epid);
    plugin_t *hog   = pm_plugin(&g_pm, (uint32_t)hpid);

    float *good_out = map_io(good);
    map_io(crash);
    map_io(evil);
    map_io(hog);

    if (plugin_call_init(good, RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK ||
        plugin_call_init(hog,  RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK)
        return 0;

    /* The hog's budget policy: three consecutive offences kill (issue #78). */
    budget_t hog_pol;
    budget_init(&hog_pol, g_hog_budget, 3);
    uint32_t hog_preempts = 0, hog_muted = 0;
    uint64_t dt_min = ~0ull;
    int kill_logged = 0;

    uint32_t sound_blocks = 0;
    int crash_killed = 0, evil_killed = 0;

    for (uint32_t b = 0; b < TOTAL_BLOCKS; b++) {
        /* The DAC path: the good plugin renders into a freshly cleared buffer
         * and we read real audio back.  process_block returns void, so success
         * is judged by the audio it produced, not by an exit code. */
        block_clear(good_out);
        run_block(good);
        if (block_has_sound(good_out))
            sound_blocks++;

        /* Fire the hostile plugins mid-stream; a fault or a forbidden syscall
         * returns -1 from the isolated run, so each must be contained. */
        if (b == TRIGGER_BLOCK) {
            crash_killed = (run_block(crash) == -1);
            evil_killed  = (run_block(evil)  == -1);
        }

        /* From the trigger on, the hog runs every block under its budget: it
         * never returns on its own - only the budget timer gets it back. */
        if (b >= TRIGGER_BLOCK && !hog_pol.killed) {
            uint64_t t0 = rd_cntpct();
            budget_arm(hog_pol.cycles);
            long r = run_block(hog);
            budget_disarm();
            uint64_t dt = rd_cntpct() - t0;

            int over = (r == BUDGET_PREEMPTED);
            if (over) {
                hog_preempts++;
                if (dt < dt_min)
                    dt_min = dt;
            }
            int act = budget_account(&hog_pol, over);
            if (act == BUDGET_MUTE)
                hog_muted++;            /* the host emits silence downstream */
            if (act == BUDGET_KILL && !kill_logged) {
                kill_logged = 1;
                uart_printf("  [budget] kill pid=%u (hog) after %u consecutive offences (last=%uus budget=%uus)\r\n",
                            (unsigned)hpid, (unsigned)hog_pol.streak,
                            (unsigned)lat_cyc_to_us(dt, g_freq),
                            (unsigned)lat_cyc_to_us(hog_pol.cycles, g_freq));
            }
        }
    }

    pm_unload(&g_pm, (uint32_t)gpid);
    pm_unload(&g_pm, (uint32_t)cpid);
    pm_unload(&g_pm, (uint32_t)epid);
    pm_unload(&g_pm, (uint32_t)hpid);

    /* The hog ran on blocks TRIGGER..TRIGGER+2: preempted at its budget
     * boundary every time (it never returns on its own; dt_min proves the
     * budget did it), muted on the first two offences, killed on the third. */
    int hog_ok = (hog_preempts == 3) && (hog_muted == 2) &&
                 hog_pol.killed && kill_logged &&
                 (dt_min >= g_hog_budget) && (dt_min < 3 * g_hog_budget);

    return (sound_blocks == TOTAL_BLOCKS) && crash_killed && evil_killed &&
           hog_ok;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt resilience demo (issue #36 + #79) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    /* The budget timer (issue #78): CPU0's banked generic timer preempts an
     * over-budget plugin at EL0.  No cadence timer runs in this demo - the
     * only CNTP use is the budget window around the hog. */
    gic_init();
    gic_enable_irq(TIMER_IRQ);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(g_freq));
    uint64_t block_cycles = (g_freq * RING_BLOCK) / RING_SR;
    g_hog_budget = budget_fair_share(block_cycles, 4);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "good",  good_elf_start,  (size_t)(good_elf_end  - good_elf_start));
    pm_register_blob(&g_pm, "crash", crash_elf_start, (size_t)(crash_elf_end - crash_elf_start));
    pm_register_blob(&g_pm, "evil",  evil_elf_start,  (size_t)(evil_elf_end  - evil_elf_start));
    pm_register_blob(&g_pm, "hog",   hog_elf_start,   (size_t)(hog_elf_end   - hog_elf_start));
    uart_printf("hog budget: %uus of a %uus block (fair share of 4)\r\n",
                (unsigned)lat_cyc_to_us(g_hog_budget, g_freq),
                (unsigned)lat_cyc_to_us(block_cycles, g_freq));

    size_t baseline = pmm_free_pages();

    int passes = 0;
    for (int i = 0; i < 10; i++) {
        int ok = demo_pass();
        if (ok) passes++;
        uart_printf("  run %d: good-audio-intact + all-three-neutralised = %s\r\n",
                    i + 1, ok ? "yes" : "NO");
    }

    size_t after = pmm_free_pages();
    int no_leak = (after == baseline);
    uart_printf("leak: baseline=%u after-10x=%u no-leak=%d\r\n",
                (unsigned)baseline, (unsigned)after, no_leak);
    uart_printf("checks: passes=%d/10 no-leak=%d\r\n", passes, no_leak);
    uart_puts((passes == 10 && no_leak) ? "RESILIENCE: PASS\r\n"
                                        : "RESILIENCE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
