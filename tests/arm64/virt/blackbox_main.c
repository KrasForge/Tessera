/* tests/arm64/virt/blackbox_main.c - crash black-box on QEMU 'virt'
 * (Theme A: reliability).
 *
 * When a plugin faults, isolation catches and kills it while the engine keeps
 * running - but a plain reboot loses the evidence.  The black box records the
 * last N DAC-bound blocks; when the plugin is killed it freezes those blocks
 * plus the faulting plugin's identity and cause, and serialises the snapshot to
 * a store that survives the reboot, so a post-mortem can recover what happened.
 *
 * The harness proves the whole path, end to end and for real:
 *   1. play blocks through the reference effect, recording each into the black
 *      box;
 *   2. the effect takes a genuine EL0 fault (the M8 crash plugin; the MMU traps
 *      it and the isolated run returns -1) - the fault is really contained;
 *   3. capture the snapshot (faulting pid, name, cause = MMU) and serialise it
 *      to a reserved store;
 *   4. "reboot" - a fresh, zeroed recorder - and parse the store back;
 *   5. the recovered snapshot has the faulting plugin's identity and the last N
 *      pre-crash blocks, bit-exact; a corrupted copy of the store is rejected.
 *
 * Built MMU-on (virt_mmu.ld), single core.  FP-free: the effect runs its DSP at
 * EL0; the harness feeds a fixed tone as raw float bit-patterns, records and
 * compares blocks as raw words, and serialises with an integer checksum.
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
#include "blackbox.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char effect_elf_start[], effect_elf_end[];
extern char crash_elf_start[],  crash_elf_end[];

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

#define PRE   6u          /* blocks recorded before the fault (> BB_BLOCKS) */

/* Strictly-positive tone as raw float bits (0.5, 0.3, 0.1, 0.3): the DC
 * component keeps the low-pass output non-silent, the variation makes it a real
 * signal.  No FP here - the plugin does the DSP at EL0. */
static const uint32_t TONE[4] = { 0x3F000000u, 0x3E99999Au, 0x3DCCCCCDu, 0x3E99999Au };

static blackbox_t g_bb;               /* live recorder                       */
static blackbox_t g_recovered;        /* post-reboot parse target            */
static uint8_t    g_store[16384];     /* the reboot-surviving store           */
static uint8_t    g_corrupt[16384];

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
    uart_puts("\r\n=== QEMU virt crash black-box (Theme A: reliability) ===\r\n");

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
    if (epid <= 0 || cpid <= 0) { uart_puts("BLACK-BOX: FAIL\r\n"); psci_system_off(); for(;;); }
    plugin_t *eff   = pm_plugin(&g_pm, (uint32_t)epid);
    plugin_t *crash = pm_plugin(&g_pm, (uint32_t)cpid);

    uintptr_t eff_out_pa = phys_alloc_page_zero();
    uintptr_t eff_in_pa  = phys_alloc_page_zero();
    plugin_map_region(eff, RESULTS_VA, eff_out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(eff, RING_IN_VA, eff_in_pa,  PAGE_SIZE, VMM_READ);
    uint32_t *eff_out_k = (uint32_t *)P2V(eff_out_pa);
    uint32_t *eff_in_k  = (uint32_t *)P2V(eff_in_pa);
    for (uint32_t i = 0; i < RING_BLOCK; i++) {
        eff_in_k[i]              = TONE[i & 3u];
        eff_in_k[RING_BLOCK + i] = TONE[i & 3u];
    }

    /* The crash plugin needs mapped I/O pages before it faults. */
    uintptr_t c_out = phys_alloc_page_zero(), c_in = phys_alloc_page_zero();
    plugin_map_region(crash, RESULTS_VA, c_out, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(crash, RING_IN_VA, c_in,  PAGE_SIZE, VMM_READ);

    plugin_call_init(eff, RING_SR, RING_BLOCK);
    bb_init(&g_bb, WORDS);

    /* 1. Play and record. */
    uint32_t sound = 0;
    for (uint32_t b = 0; b < PRE; b++) {
        run_block(eff);
        bb_record(&g_bb, eff_out_k);
        if (has_sound(eff_out_k)) sound++;
    }

    /* 2. The effect takes a genuine, MMU-caught EL0 fault. */
    int fault_caught = (run_block(crash) == -1);

    /* 3. Capture and persist the snapshot. */
    bb_capture(&g_bb, (uint32_t)epid, "effect", BB_CAUSE_MMU);
    size_t n = bb_serialize(&g_bb, g_store, sizeof g_store);

    /* 4. Reboot: a fresh recorder recovers from the store alone. */
    bb_init(&g_recovered, 1u);
    int parsed = bb_parse(g_store, n, &g_recovered);

    /* 5. Verify the recovered post-mortem. */
    int id_ok = parsed &&
                (g_recovered.fault_pid == (uint32_t)epid) &&
                (g_recovered.fault_cause == (uint32_t)BB_CAUSE_MMU) &&
                (g_recovered.fault_block == PRE) &&
                (g_recovered.fault_name[0] == 'e' && g_recovered.fault_name[5] == 't');
    int blocks_ok = parsed && (g_recovered.count == BB_BLOCKS);
    uint32_t a[WORDS], b[WORDS];
    for (uint32_t i = 0; i < BB_BLOCKS && blocks_ok; i++) {
        if (!bb_chrono(&g_bb, i, a) || !bb_chrono(&g_recovered, i, b)) { blocks_ok = 0; break; }
        for (uint32_t w = 0; w < WORDS; w++) if (a[w] != b[w]) { blocks_ok = 0; break; }
    }

    /* corruption must be rejected */
    for (size_t i = 0; i < n; i++) g_corrupt[i] = g_store[i];
    g_corrupt[n / 2] ^= 0x40;
    int corrupt_rejected = (bb_parse(g_corrupt, n, &g_recovered) == 0);

    pm_unload(&g_pm, (uint32_t)epid);
    pm_unload(&g_pm, (uint32_t)cpid);

    uart_printf("recorded=%u/%u never-silent=%u/%u fault-caught=%d serialized=%u bytes\r\n",
                (unsigned)g_bb.total, (unsigned)PRE, (unsigned)sound, (unsigned)PRE,
                fault_caught, (unsigned)n);
    uart_printf("recovered: pid=%u cause=%u block=%u name0=%c count=%u\r\n",
                (unsigned)g_recovered.fault_pid, (unsigned)g_recovered.fault_cause,
                (unsigned)g_recovered.fault_block, g_recovered.fault_name[0],
                (unsigned)g_recovered.count);
    uart_printf("checks: identity=%d blocks-bit-exact=%d corrupt-rejected=%d\r\n",
                id_ok, blocks_ok, corrupt_rejected);

    int ok = (sound == PRE) && fault_caught && (n > 0) && parsed &&
             id_ok && blocks_ok && corrupt_rejected;

    uart_puts(ok ? "BLACK-BOX: PASS\r\n" : "BLACK-BOX: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
