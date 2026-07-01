/* tests/arm64/virt/sdk_main.c - prove an SDK-built plugin loads and produces
 * audio under Tessera (Issue #38).
 *
 * The plugin under test (sdk/examples/sine_plugin) was built with ONLY the
 * published SDK - no kernel headers.  This harness loads that ELF like any
 * other plugin and checks the SDK acceptance end to end:
 *
 *   1. It loads into an isolated, sandboxed address space and its sandbox
 *      audits clean (the SDK link script is W^X-safe: no writable+executable
 *      page, no mapping outside the plugin's own regions).
 *   2. Its process_block produces real (non-silent) stereo audio.
 *   3. A parameter change delivered through the host queue (id 0 = frequency,
 *      440 -> 880 Hz) is picked up by the SDK's tessera_param_queue_read() and
 *      changes the output.
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
#include "sandbox.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char sdk_sine_elf_start[], sdk_sine_elf_end[];

static void *ring_new(void *c)                              { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

/* IEEE-754 bit patterns (the harness is -mgeneral-regs-only: no FP here). */
#define FREQ_880_BITS 0x44540000u        /* 880.0f */

#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

static int block_has_sound(const uint32_t *w)
{
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        if (w[i] != 0u)
            return 1;
    return 0;
}

static uint32_t block_hash(const uint32_t *w)
{
    uint32_t h = 2166136261u;            /* FNV-ish over the raw words */
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        h = (h ^ w[i]) * 16777619u;
    return h;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M9 SDK plugin (issue #38) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "sine", sdk_sine_elf_start,
                     (size_t)(sdk_sine_elf_end - sdk_sine_elf_start));

    long pid = pm_load(&g_pm, "sine");
    uart_printf("load: sine -> %d\r\n", (int)pid);
    plugin_t *pl = pm_plugin(&g_pm, (uint32_t)pid);
    int load_ok = (pid > 0) && pl;

    /* ---- 1. sandbox audit clean ---- */
    int audit_ok = 0;
    if (load_ok) {
        uintptr_t out_pa = phys_alloc_page_zero();
        uintptr_t in_pa  = phys_alloc_page_zero();
        plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
        plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
        volatile uint32_t *out = (volatile uint32_t *)P2V(out_pa);

        sandbox_region_t allow[16];
        int n = plugin_sandbox_regions(pl, allow, 16);
        sandbox_report_t rep;
        int rc = sandbox_audit(pl->proc, allow, n, &rep);
        uart_printf("audit: regions=%d pages=%d outside=%d device=%d wx=%d rc=%d\r\n",
                    n, rep.total_pages, rep.outside_pages, rep.device_pages,
                    rep.wx_pages, rc);
        audit_ok = (rc == 0) && (rep.wx_pages == 0) && (rep.outside_pages == 0);

        /* ---- 2. produces audio ---- */
        plugin_call_init(pl, RING_SR, RING_BLOCK);
        plugin_call_block(pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
        int sound_a = block_has_sound((const uint32_t *)out);
        uint32_t hash_a = block_hash((const uint32_t *)out);

        /* ---- 3. parameter change via the host queue changes the output ---- */
        pm_set_param(&g_pm, (uint32_t)pid, 0u, FREQ_880_BITS);   /* freq = 880 Hz */
        plugin_call_block(pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
        int sound_b = block_has_sound((const uint32_t *)out);
        uint32_t hash_b = block_hash((const uint32_t *)out);

        uart_printf("audio: sound_a=%d sound_b=%d changed=%d\r\n",
                    sound_a, sound_b, (hash_a != hash_b));
        int audio_ok = sound_a && sound_b && (hash_a != hash_b);

        uart_printf("checks: load=%d audit=%d audio=%d\r\n",
                    load_ok, audit_ok, audio_ok);
        uart_puts((load_ok && audit_ok && audio_ok) ? "SDK: PASS\r\n"
                                                    : "SDK: FAIL\r\n");
    } else {
        uart_puts("SDK: FAIL\r\n");
    }

    for (;;)
        __asm__ volatile("wfe");
}
