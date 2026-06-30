/* tests/arm64/virt/plugin_main.c - plugin loader test on QEMU 'virt'
 * (Issue #24, M5).
 *
 * Brings up the real pmm/mmu/process/exception stack with the MMU enabled,
 * then loads two plugin ELFs embedded in the image:
 *
 *   pass  - a passthrough plugin.  It must load into its own address space,
 *           run plugin_init at EL0 via the trampoline, and return cleanly
 *           (exit code TESSERA_PLUGIN_OK), proving the load + EL0 entry path.
 *   evil  - a plugin whose plugin_init writes to a kernel address.  The MMU
 *           maps the kernel as EL1-only in the plugin space, so the store
 *           faults and the host kills the plugin (state PROC_KILLED) - kernel
 *           memory is never touched.
 *
 * Also confirms the plugin ELF is self-contained (zero unresolved imports).
 */

#include "pmm.h"
#include "mmu.h"
#include "process.h"
#include "exceptions.h"
#include "elf64.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char pass_elf_start[], pass_elf_end[];
extern char evil_elf_start[], evil_elf_end[];

/* Allow EL0/EL1 floating point so a plugin that uses NEON does not trap (the
 * audio path uses floats; lazy-FPU context switching is issue #15's concern,
 * not the loader's). */
static void enable_fp(void)
{
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ull << 20);                 /* FPEN = 0b11: no trap */
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(cpacr));
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt plugin loader (issue #24) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();
    enable_fp();

    size_t pass_len = (size_t)(pass_elf_end - pass_elf_start);
    size_t evil_len = (size_t)(evil_elf_end - evil_elf_start);
    uart_printf("pass ELF %u bytes, evil ELF %u bytes\r\n",
                (unsigned)pass_len, (unsigned)evil_len);

    /* ---- self-containment: no unresolved imports ---- */
    int imports = elf64_undefined_count(pass_elf_start, pass_len);
    uart_printf("pass ELF unresolved imports = %d (expect 0)\r\n", imports);

    /* ---- load + run the passthrough plugin ---- */
    plugin_t pass;
    int lr = plugin_load(&pass, pass_elf_start, pass_len, "pass");
    uart_printf("plugin_load(pass) = %d, init_va=0x%x:%x\r\n", lr,
                (unsigned)(pass.init_va >> 32), (unsigned)pass.init_va);

    long code = -999;
    if (lr == PLUGIN_OK)
        code = plugin_call_init(&pass, 48000, 128);
    uart_printf("pass plugin_init -> %d, state=%d (3=ZOMBIE)\r\n",
                (int)code, pass.proc ? (int)pass.proc->state : -1);

    /* ---- load + run the misbehaving plugin (must be killed) ---- */
    plugin_t evil;
    int er = plugin_load(&evil, evil_elf_start, evil_len, "evil");
    long ecode = -999;
    if (er == PLUGIN_OK)
        ecode = plugin_call_init(&evil, 48000, 128);
    uart_printf("evil plugin_init -> %d, state=%d (4=KILLED)\r\n",
                (int)ecode, evil.proc ? (int)evil.proc->state : -1);

    int pass_ok = (lr == PLUGIN_OK) && (code == TESSERA_PLUGIN_OK) &&
                  (pass.proc->state == PROC_ZOMBIE) && (imports == 0);
    int evil_killed = (er == PLUGIN_OK) && (ecode == -1) &&
                      (evil.proc->state == PROC_KILLED);

    uart_printf("checks: pass-loaded-and-init=%d evil-killed=%d\r\n",
                pass_ok, evil_killed);

    uart_puts((pass_ok && evil_killed) ? "PLUGIN-LOAD: PASS\r\n"
                                       : "PLUGIN-LOAD: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
