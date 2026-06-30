/* tests/arm64/virt/sandbox_main.c - M8 plugin sandbox on QEMU 'virt'
 * (Issue #35).
 *
 * Proves the plugin sandbox is airtight, both statically and at runtime:
 *
 *   1. sandbox_audit() of a freshly loaded plugin is clean - every mapped page
 *      lies in the plugin's own regions (code/rodata/data/BSS/stack/param/
 *      trampoline), with no device (MMIO) and no W^X pages, and nothing else.
 *   2. The audit catches an unexpected mapping: injecting one extra page that
 *      is not on the allowlist is reported as an outside-the-sandbox page.
 *   3. A plugin that issues an SVC from its own body (not the controlled
 *      trampoline) is killed - the syscall is never serviced.
 *   4. A plugin that touches memory it was never granted (a "malloc" pointer
 *      that is not mapped) takes a data abort and is killed by the MMU.
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
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char pass_elf_start[],  pass_elf_end[];
extern char sbsvc_elf_start[], sbsvc_elf_end[];
extern char sbmem_elf_start[], sbmem_elf_end[];

/* Graph control plane (pm_init wants one); rings are unused here. */
static void *ring_new(void *c)                              { (void)c; return (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

/* Strong syscall handlers (a plugin SVC from the body must be killed, not
 * routed here - these only back the sanctioned trampoline exit path). */
long sys_plugin_load(const char *p)                            { return pm_load(&g_pm, p); }
long sys_plugin_unload(uint32_t pid)                           { return pm_unload(&g_pm, pid); }
long sys_plugin_set_param(uint32_t pid, uint32_t id, uint32_t b){ return pm_set_param(&g_pm, pid, id, b); }

#define INJECT_VA (USER_VA_BASE + 0x20000000ull)   /* not in any plugin region */

int main_audit_clean(plugin_t *pl, sandbox_region_t *allow, int *n_out)
{
    int n = plugin_sandbox_regions(pl, allow, 16);
    *n_out = n;
    sandbox_report_t rep;
    int rc = sandbox_audit(pl->proc, allow, n, &rep);
    uart_printf("audit: regions=%d pages=%d outside=%d device=%d wx=%d rc=%d\r\n",
                n, rep.total_pages, rep.outside_pages, rep.device_pages,
                rep.wx_pages, rc);
    return (rc == 0) && (rep.total_pages > 0) && (rep.outside_pages == 0) &&
           (rep.device_pages == 0) && (rep.wx_pages == 0);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt M8 plugin sandbox (issue #35) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_register_blob(&g_pm, "pass",  pass_elf_start,  (size_t)(pass_elf_end  - pass_elf_start));
    pm_register_blob(&g_pm, "sbsvc", sbsvc_elf_start, (size_t)(sbsvc_elf_end - sbsvc_elf_start));
    pm_register_blob(&g_pm, "sbmem", sbmem_elf_start, (size_t)(sbmem_elf_end - sbmem_elf_start));

    /* ---- 1. a freshly loaded plugin audits clean ---- */
    long pid = pm_load(&g_pm, "pass");
    plugin_t *pl = pm_plugin(&g_pm, (uint32_t)pid);
    sandbox_region_t allow[16];
    int n_allow = 0;
    int clean_ok = (pid > 0) && pl && main_audit_clean(pl, allow, &n_allow);

    /* ---- 2. the audit catches an unexpected mapping ---- */
    /* Inject a page directly into the address space, bypassing the loader's
     * region bookkeeping, so it is NOT on the allowlist. */
    uintptr_t extra = phys_alloc_page_zero();
    process_map(pl->proc, extra, INJECT_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);
    sandbox_report_t rep2;
    int rc2 = sandbox_audit(pl->proc, allow, n_allow, &rep2);
    uart_printf("inject: outside=%d first_bad=0x%x rc=%d\r\n",
                rep2.outside_pages, (unsigned)(rep2.first_bad_va & 0xFFFFFFFF), rc2);
    int detect_ok = (rc2 < 0) && (rep2.outside_pages == 1) &&
                    (rep2.first_bad_va == INJECT_VA);

    /* ---- 3. SVC from the plugin body is fatal ---- */
    long svc_pid = pm_load(&g_pm, "sbsvc");
    long svc_ret = plugin_call_init(pm_plugin(&g_pm, (uint32_t)svc_pid),
                                    48000u, 64u);
    uart_printf("svc-body: load=%d init=%d (expect init=-1)\r\n",
                (int)svc_pid, (int)svc_ret);
    int svc_kill_ok = (svc_pid > 0) && (svc_ret == -1);

    /* ---- 4. out-of-sandbox memory access is fatal ---- */
    long mem_pid = pm_load(&g_pm, "sbmem");
    long mem_ret = plugin_call_init(pm_plugin(&g_pm, (uint32_t)mem_pid),
                                    48000u, 64u);
    uart_printf("oob-mem: load=%d init=%d (expect init=-1)\r\n",
                (int)mem_pid, (int)mem_ret);
    int mem_kill_ok = (mem_pid > 0) && (mem_ret == -1);

    uart_printf("checks: clean=%d detect=%d svc-kill=%d mem-kill=%d\r\n",
                clean_ok, detect_ok, svc_kill_ok, mem_kill_ok);
    uart_puts((clean_ok && detect_ok && svc_kill_ok && mem_kill_ok)
                  ? "SANDBOX: PASS\r\n" : "SANDBOX: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
