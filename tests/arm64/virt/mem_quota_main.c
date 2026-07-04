/* tests/arm64/virt/mem_quota_main.c - per-plugin memory quota on QEMU 'virt'
 * (Theme A: reliability).
 *
 * The memory leg of the sandbox, alongside the M12 CPU budget.  A plugin's
 * footprint is fixed at load, and plugins cannot allocate at runtime, so the
 * quota is enforced on the image's declared page count before a single frame is
 * committed: a greedy or hostile image is refused outright and cannot exhaust
 * physical memory or starve the audio engine and the other plugins.
 *
 * The harness sets a per-plugin budget and shows, deterministically:
 *   - the small reference effect loads (under budget) and its process_block
 *     runs without fault;
 *   - the greedy plugin (a large .bss) is refused with PM_EQUOTA, and not one
 *     physical page was committed for it (the free count is unchanged across
 *     the rejected load);
 *   - with the budget lifted, the same greedy plugin loads fine (a positive
 *     control: it was the quota, not a broken image);
 *   - unloading everything returns the frame allocator exactly to baseline
 *     (no leak).
 *
 * Built MMU-on (virt_mmu.ld), single core.
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
#include "elf64.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char effect_elf_start[], effect_elf_end[];
extern char greedy_elf_start[], greedy_elf_end[];

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

#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

#define QUOTA_PAGES 32u        /* per-plugin budget: fits the effect, not the greedy */

/* Map a loaded plugin's de-interleaved I/O pages so its process_block can run. */
static void map_io(plugin_t *pl)
{
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt per-plugin memory quota (Theme A: reliability) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));
    pm_register_blob(&g_pm, "greedy", greedy_elf_start, (size_t)(greedy_elf_end - greedy_elf_start));

    uint32_t eff_pages = elf64_load_pages(effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));
    uint32_t grd_pages = elf64_load_pages(greedy_elf_start, (size_t)(greedy_elf_end - greedy_elf_start));
    uart_printf("declared PT_LOAD pages: effect=%u greedy=%u  quota=%u pages/plugin\r\n",
                (unsigned)eff_pages, (unsigned)grd_pages, (unsigned)QUOTA_PAGES);

    size_t baseline = pmm_free_pages();

    pm_set_quota(&g_pm, QUOTA_PAGES);

    /* 1. The small effect fits the budget and runs. */
    long e = pm_load(&g_pm, "effect");
    int effect_ok = (e > 0);
    int effect_runs = 0;
    if (effect_ok) {
        plugin_t *eff = pm_plugin(&g_pm, (uint32_t)e);
        map_io(eff);
        plugin_call_init(eff, RING_SR, RING_BLOCK);
        effect_runs = (plugin_call_block(eff, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK) != -1);
    }

    /* 2. The greedy plugin is refused - and nothing was committed for it. */
    size_t before = pmm_free_pages();
    long g = pm_load(&g_pm, "greedy");
    size_t after = pmm_free_pages();
    int greedy_rejected = (g == PM_EQUOTA);
    int no_alloc_on_reject = (after == before);

    /* 3. Positive control: lift the budget and the same image loads fine. */
    pm_set_quota(&g_pm, 0);
    long g2 = pm_load(&g_pm, "greedy");
    int greedy_ok_unlimited = (g2 > 0);
    if (g2 > 0) pm_unload(&g_pm, (uint32_t)g2);

    if (effect_ok) pm_unload(&g_pm, (uint32_t)e);

    size_t final = pmm_free_pages();
    int no_leak = (final == baseline);

    uart_printf("effect: load=%d runs=%d\r\n", (int)e, effect_runs);
    uart_printf("greedy: rejected=%d (rc=%d) no-alloc-on-reject=%d loads-when-unlimited=%d\r\n",
                greedy_rejected, (int)g, no_alloc_on_reject, greedy_ok_unlimited);
    uart_printf("leak: baseline=%u after=%u no-leak=%d\r\n",
                (unsigned)baseline, (unsigned)final, no_leak);

    int ok = effect_ok && effect_runs && greedy_rejected && no_alloc_on_reject &&
             greedy_ok_unlimited && no_leak;
    uart_puts(ok ? "MEM-QUOTA: PASS\r\n" : "MEM-QUOTA: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
