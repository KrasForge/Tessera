/* tests/arm64/virt/safe_bypass_main.c - never-go-silent safe-mode bypass on
 * QEMU 'virt' (Theme A: reliability).
 *
 * The audible payoff of the isolation architecture.  A signal path
 * input -> effect -> DAC carries real audio through the M6 reference low-pass.
 * Partway through, the effect suffers a fatal fault (a real EL0 MMU data abort,
 * injected via the M8 crash plugin - the same containment path the resilience
 * demo exercises).  Without safe-mode bypass the DAC, whose producer just died,
 * would go silent: a pedal dead on stage.  Instead the platform detects the
 * dead effect and routes its dry input straight to the DAC.
 *
 * The harness asserts the deterministic, product-relevant facts:
 *   - the DAC is never silent - every block carries sound;
 *   - before the fault, the DAC carries the effect's output, which the filter
 *     has altered away from the dry input (so the signal really came from the
 *     live effect);
 *   - the fault is genuinely caught (the isolated run returns -1);
 *   - after the fault, the DAC carries the dry input, bit-exactly (clean
 *     bypass), for every remaining block.
 *
 * Built MMU-on (virt_mmu.ld), single core.  FP-free: the effect runs at EL0,
 * the test tone is filled by the FP-compiled sb_conv.c, and the harness moves
 * plugin pages as raw words.
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
#include "safe_bypass.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);
void sb_fill_tone(void *page, uint32_t frames);   /* sb_conv.c (FP) */

extern char effect_elf_start[], effect_elf_end[];
extern char crash_elf_start[],  crash_elf_end[];

/* PSCI SYSTEM_OFF: clean shutdown so the log flushes and QEMU exits promptly. */
static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

/* Graph control plane (pm_init wants one); edge rings are unused - we drive
 * the effect's process_block directly, as the resilience demo does. */
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

static long run_block(plugin_t *pl)
{
    return plugin_call_block(pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
}

static int has_sound(const uint32_t *w)
{ for (uint32_t i = 0; i < WORDS; i++) if (w[i]) return 1; return 0; }
static int words_eq(const uint32_t *a, const uint32_t *b)
{ for (uint32_t i = 0; i < WORDS; i++) if (a[i] != b[i]) return 0; return 1; }

#define TOTAL    8u
#define TRIGGER  4u          /* the effect faults here ("after 4 blocks") */

static uint32_t g_dac[WORDS];

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt safe-mode bypass (Theme A: reliability) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));
    pm_register_blob(&g_pm, "crash",  crash_elf_start,  (size_t)(crash_elf_end  - crash_elf_start));

    long epid = pm_load(&g_pm, "effect");
    long cpid = pm_load(&g_pm, "crash");
    if (epid <= 0 || cpid <= 0) { uart_puts("SAFE-BYPASS: FAIL\r\n"); psci_system_off(); for(;;); }
    plugin_t *eff   = pm_plugin(&g_pm, (uint32_t)epid);
    plugin_t *crash = pm_plugin(&g_pm, (uint32_t)cpid);

    /* The effect's de-interleaved input/output pages; keep kernel views of both
     * (input is the dry signal the bypass falls back to). */
    uintptr_t eff_out_pa = phys_alloc_page_zero();
    uintptr_t eff_in_pa  = phys_alloc_page_zero();
    plugin_map_region(eff, RESULTS_VA, eff_out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(eff, RING_IN_VA, eff_in_pa,  PAGE_SIZE, VMM_READ);
    uint32_t *eff_out_k = (uint32_t *)P2V(eff_out_pa);
    uint32_t *eff_in_k  = (uint32_t *)P2V(eff_in_pa);

    /* The crash plugin still needs mapped I/O pages before it faults. */
    uintptr_t c_out = phys_alloc_page_zero(), c_in = phys_alloc_page_zero();
    plugin_map_region(crash, RESULTS_VA, c_out, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(crash, RING_IN_VA, c_in,  PAGE_SIZE, VMM_READ);

    plugin_call_init(eff, RING_SR, RING_BLOCK);

    sb_state_t sb;
    sb_init(&sb);

    uint32_t sound = 0, dry_ok = 0, from_effect = 0, filt_differs = 0;
    int fault_caught = 0, effect_dead = 0;

    for (uint32_t b = 0; b < TOTAL; b++) {
        sb_fill_tone(eff_in_k, RING_BLOCK);   /* the graph input feeds the effect */

        if (b == TRIGGER) {
            /* The effect's process_block hits a fatal fault; the MMU traps it
             * from EL0, the isolated run returns -1, and the effect is dead. */
            fault_caught = (run_block(crash) == -1);
            effect_dead  = 1;
        } else if (!effect_dead) {
            run_block(eff);                    /* the live effect renders a block */
        }

        int bypassed = sb_resolve(&sb, !effect_dead, eff_out_k, eff_in_k, g_dac, WORDS);

        if (has_sound(g_dac)) sound++;
        if (!bypassed) {
            if (words_eq(g_dac, eff_out_k))       from_effect++;   /* DAC == live output */
            if (!words_eq(eff_out_k, eff_in_k))   filt_differs++;  /* filter altered it  */
        } else {
            if (words_eq(g_dac, eff_in_k))        dry_ok++;        /* DAC == dry input   */
        }
    }

    pm_unload(&g_pm, (uint32_t)epid);
    pm_unload(&g_pm, (uint32_t)cpid);

    uart_printf("blocks=%u trigger=%u  normal=%u bypass=%u\r\n",
                (unsigned)TOTAL, (unsigned)TRIGGER,
                (unsigned)sb.normal_blocks, (unsigned)sb.bypass_blocks);
    uart_printf("never-silent=%u/%u  from-live-effect=%u/%u  filter-altered=%u/%u  "
                "dry-passthrough=%u/%u  fault-caught=%u\r\n",
                (unsigned)sound, (unsigned)TOTAL,
                (unsigned)from_effect, (unsigned)TRIGGER,
                (unsigned)filt_differs, (unsigned)TRIGGER,
                (unsigned)dry_ok, (unsigned)(TOTAL - TRIGGER),
                (unsigned)fault_caught);

    int ok = (sound == TOTAL) &&
             (sb.normal_blocks == TRIGGER) && (sb.bypass_blocks == TOTAL - TRIGGER) &&
             (from_effect == TRIGGER) && (filt_differs == TRIGGER) &&
             (dry_ok == TOTAL - TRIGGER) && fault_caught;

    uart_puts(ok ? "SAFE-BYPASS: PASS\r\n" : "SAFE-BYPASS: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
