/* tests/arm64/virt/control_main.c - M7 control syscalls on QEMU 'virt'
 * (Issue #30).
 *
 * Exercises the consolidated control surface end to end:
 *
 *   1. Leak test - pm_load("pass") + pm_unload() 100 times and confirm the
 *      physical allocator's free-page count returns exactly to its baseline.
 *   2. Parameter delivery - load an echo plugin, sys_plugin_set_param into its
 *      lock-free queue, run it, and confirm it received the (id, value) within
 *      one block.
 *   3. SVC surface - an EL0 control client issues all five syscalls
 *      (load / set_param / connect / disconnect / unload) and the kernel checks
 *      the return codes.
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
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char pass_elf_start[], pass_elf_end[];
extern char echo_elf_start[], echo_elf_end[];
extern char ctl2_elf_start[], ctl2_elf_end[];

/* Edge rings for the graph control plane. */
static void *ring_new(void *ctx)
{
    (void)ctx;
    size_t pages = (arb_bytes(RING_FRAMES) + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t pa = phys_alloc_contig(pages);
    if (!pa) return (void *)0;
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)P2V(pa);
    arb_init(h, RING_FRAMES);
    return h;
}
static void ring_del(void *ctx, void *r)             { (void)ctx; (void)r; }
static int  ring_map(void *c, uint32_t p, void *r, int in) { (void)c;(void)p;(void)r;(void)in; return 0; }
static void ring_unmap(void *c, uint32_t p, void *r, int in){ (void)c;(void)p;(void)r;(void)in; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

/* Strong syscall handlers, backed by the manager / control plane. */
long sys_plugin_load(const char *path)                         { return pm_load(&g_pm, path); }
long sys_plugin_unload(uint32_t pid)                           { return pm_unload(&g_pm, pid); }
long sys_plugin_set_param(uint32_t pid, uint32_t id, uint32_t b){ return pm_set_param(&g_pm, pid, id, b); }
long sys_graph_connect(uint32_t s, uint32_t d)                 { return pm_connect(&g_pm, s, d); }
long sys_graph_disconnect(uint32_t s, uint32_t d)              { return pm_disconnect(&g_pm, s, d); }

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M7 control syscalls (issue #30) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    gc_add_dac(&g_gc);
    pm_register_blob(&g_pm, "pass", pass_elf_start, (size_t)(pass_elf_end - pass_elf_start));
    pm_register_blob(&g_pm, "echo", echo_elf_start, (size_t)(echo_elf_end - echo_elf_start));

    /* ---- 1. leak test: 100x load/unload returns to baseline ---- */
    size_t base = pmm_free_pages();
    int load_ok = 1;
    for (int i = 0; i < 100; i++) {
        long pid = pm_load(&g_pm, "pass");
        if (pid <= 0) { load_ok = 0; break; }
        if (pm_unload(&g_pm, (uint32_t)pid) != PM_OK) { load_ok = 0; break; }
    }
    size_t after = pmm_free_pages();
    uart_printf("leak: baseline=%u after-100x=%u load_ok=%d\r\n",
                (unsigned)base, (unsigned)after, load_ok);
    int no_leak = load_ok && (after == base);

    /* ---- 2. parameter delivery within one block ---- */
    long ep = pm_load(&g_pm, "echo");
    uintptr_t eres_pa = phys_alloc_page_zero();
    volatile uint32_t *eres = (volatile uint32_t *)P2V(eres_pa);
    plugin_map_region(pm_plugin(&g_pm, (uint32_t)ep), RESULTS_VA, eres_pa,
                      PAGE_SIZE, VMM_READ | VMM_WRITE);
    pm_set_param(&g_pm, (uint32_t)ep, 7u, 0x3f000000u);    /* id 7, value 0.5f */
    plugin_call_init(pm_plugin(&g_pm, (uint32_t)ep), RING_SR, RING_BLOCK);
    uart_printf("param: drained=%u id=%u bits=0x%x (expect 1,7,0x3f000000)\r\n",
                (unsigned)eres[0], (unsigned)eres[1], (unsigned)eres[2]);
    int param_ok = (eres[0] == 1u) && (eres[1] == 7u) && (eres[2] == 0x3f000000u);
    pm_unload(&g_pm, (uint32_t)ep);

    /* ---- 3. all five syscalls callable from EL0 ---- */
    plugin_t cp;
    plugin_load(&cp, ctl2_elf_start, (size_t)(ctl2_elf_end - ctl2_elf_start), "ctl2");
    uintptr_t cres_pa = phys_alloc_page_zero();
    volatile long *cres = (volatile long *)P2V(cres_pa);
    plugin_map_region(&cp, RESULTS_VA, cres_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_call_init(&cp, RING_SR, RING_BLOCK);
    uart_printf("svc: load=%d setp=%d conn=%d disc=%d unload=%d\r\n",
                (int)cres[0], (int)cres[1], (int)cres[2], (int)cres[3], (int)cres[4]);
    int svc_ok = (cres[0] > 0) && (cres[1] == 0) && (cres[2] == 0) &&
                 (cres[3] == 0) && (cres[4] == 0);

    uart_printf("checks: no-leak=%d param=%d svc=%d\r\n", no_leak, param_ok, svc_ok);
    uart_puts((no_leak && param_ok && svc_ok) ? "CONTROL: PASS\r\n"
                                              : "CONTROL: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
