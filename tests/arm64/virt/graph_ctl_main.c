/* tests/arm64/virt/graph_ctl_main.c - graph control syscalls on QEMU 'virt'
 * (Issue #28, M6).
 *
 * An EL0 control client (loaded via the plugin loader) calls the new
 * sys_graph_connect / _disconnect / _list syscalls to rewire the audio graph
 * at runtime, writing each return value into a shared results page.  The kernel
 * services the syscalls through the real graph control plane (allocating and
 * mapping ring buffers) and then verifies, from the results page and from the
 * control-plane state, that:
 *
 *   - connect succeeded and added one edge,
 *   - a duplicate connect was rejected (no second edge),
 *   - list reported the accurate edge count after each operation,
 *   - disconnect removed the edge.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "graph_control.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char ctl_elf_start[], ctl_elf_end[];

/* ---- ring backend for the control plane (real rings, recorded maps) ---- */
static int g_maps, g_unmaps;

static void *ring_new(void *ctx)
{
    (void)ctx;
    size_t pages = (arb_bytes(RING_FRAMES) + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t pa = phys_alloc_contig(pages);
    if (!pa)
        return (void *)0;
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)P2V(pa);
    arb_init(h, RING_FRAMES);
    return h;
}
static void ring_del(void *ctx, void *r)            { (void)ctx; (void)r; }
static int  ring_map(void *ctx, uint32_t pid, void *r, int in)
{ (void)ctx; (void)pid; (void)r; (void)in; g_maps++; return 0; }
static void ring_unmap(void *ctx, uint32_t pid, void *r, int in)
{ (void)ctx; (void)pid; (void)r; (void)in; g_unmaps++; }

/* ---- the control plane, exposed to the syscall layer ---- */
static graph_control_t g_gc;

long sys_graph_connect(uint32_t s, uint32_t d)    { return gc_connect(&g_gc, s, d); }
long sys_graph_disconnect(uint32_t s, uint32_t d) { return gc_disconnect(&g_gc, s, d); }
long sys_graph_list(void)
{
    gc_edge_info_t e[GRAPH_MAX_EDGES];
    return gc_list(&g_gc, e, GRAPH_MAX_EDGES);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt graph control syscalls (issue #28) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    gc_add_plugin(&g_gc, GRAPH_SYNTH_PID);
    gc_add_dac(&g_gc);

    /* Shared results page, kernel-visible and mapped into the controller. */
    uintptr_t res_pa = phys_alloc_page_zero();
    volatile long *res = (volatile long *)P2V(res_pa);

    plugin_t ctl;
    int lr = plugin_load(&ctl, ctl_elf_start,
                         (size_t)(ctl_elf_end - ctl_elf_start), "controller");
    plugin_map_region(&ctl, RESULTS_VA, res_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    long code = plugin_call_init(&ctl, RING_SR, RING_BLOCK);

    uart_printf("controller exit=%d; results: connect=%d dup=%d list1=%d disc=%d list2=%d\r\n",
                (int)code, (int)res[0], (int)res[1], (int)res[2], (int)res[3], (int)res[4]);
    uart_printf("ring maps=%d unmaps=%d, final edges=%d gen=%u\r\n",
                g_maps, g_unmaps, (int)sys_graph_list(), (unsigned)gc_generation(&g_gc));

    int ok = (code == TESSERA_PLUGIN_OK) &&
             (res[0] == GC_OK) &&            /* connect succeeded      */
             (res[1] == GC_EEXIST) &&        /* duplicate rejected     */
             (res[2] == 1) &&                /* one edge after connect */
             (res[3] == GC_OK) &&            /* disconnect succeeded   */
             (res[4] == 0) &&                /* zero edges after       */
             (g_maps == 2) && (g_unmaps == 2) &&
             (gc_generation(&g_gc) >= 4) &&  /* two rewires (even)     */
             ((gc_generation(&g_gc) & 1u) == 0);

    uart_printf("checks: ok=%d\r\n", ok);
    uart_puts(ok ? "GRAPH-CTL: PASS\r\n" : "GRAPH-CTL: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
