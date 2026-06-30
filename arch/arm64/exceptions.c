/* arch/arm64/exceptions.c — AArch64 exception dispatch (Issue #12, M2)
 *
 * Decodes ESR_EL1 and routes synchronous exceptions (SVC, data/instruction
 * abort, undefined instruction) to the appropriate handler.  Replaces the
 * x86 C dispatch in kernel/interrupt_handlers.c with ARM ESR decoding.
 *
 * The pure ESR decoders (arm64_ec_name / arm64_ec_class) are compiled on the
 * host too (see tests/arm64/exception_test.c); the hardware-touching dispatch
 * and self-test live behind !HOSTTEST.
 */

#include "exceptions.h"
#include <stdint.h>

/* ---- pure decoders (host-testable) ----------------------------------- */

const char *arm64_ec_name(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN:       return "undefined instruction";
    case EC_SVC_A64:       return "SVC (syscall)";
    case EC_INSTR_ABORT_L: return "instruction abort (EL0)";
    case EC_INSTR_ABORT_S: return "instruction abort (EL1)";
    case EC_DATA_ABORT_L:  return "data abort (EL0)";
    case EC_DATA_ABORT_S:  return "data abort (EL1)";
    case 0x0E:             return "illegal execution state";
    case 0x18:             return "trapped MSR/MRS";
    case 0x3C:             return "BRK (AArch64)";
    default:               return "other";
    }
}

ec_class_t arm64_ec_class(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN:                          return EC_CLASS_UNKNOWN;
    case EC_SVC_A64:                          return EC_CLASS_SVC;
    case EC_INSTR_ABORT_L: case EC_INSTR_ABORT_S: return EC_CLASS_INSTR_ABORT;
    case EC_DATA_ABORT_L:  case EC_DATA_ABORT_S:  return EC_CLASS_DATA_ABORT;
    default:                                  return EC_CLASS_OTHER;
    }
}

#ifndef HOSTTEST

#include "uart_pl011.h"

extern char vectors[];

/* Provided strongly by arch/arm64/syscalls.c (weak default below). */
void arm64_user_fault(struct trapframe *tf);

/* Recoverable-trap hook used by exceptions_selftest(). */
static volatile int      g_expect_trap;
static volatile uint32_t g_trapped_ec;

static const char *const g_kind_name[16] = {
    "EL1t sync",  "EL1t IRQ",  "EL1t FIQ",  "EL1t SError",
    "EL1h sync",  "EL1h IRQ",  "EL1h FIQ",  "EL1h SError",
    "EL0 sync",   "EL0 IRQ",   "EL0 FIQ",   "EL0 SError",
    "EL0_32 sync","EL0_32 IRQ","EL0_32 FIQ","EL0_32 SError",
};

void exceptions_init(void)
{
    __asm__ volatile("msr vbar_el1, %0; isb" :: "r"(vectors));
}

static void uart_hex64(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 15; i >= 0; i--)
        uart_putc(hex[(v >> (i * 4)) & 0xF]);
}

static void dump(struct trapframe *tf, uint32_t ec, unsigned long kind)
{
    uart_printf("  vector : %s\r\n",
                kind < 16 ? g_kind_name[kind] : "?");
    uart_printf("  cause  : %s (EC=0x%x)\r\n", arm64_ec_name(ec), (unsigned)ec);
    uart_puts("  ESR=");  uart_hex64(tf->esr_el1);
    uart_puts("  ELR=");  uart_hex64(tf->elr_el1);
    uart_puts("\r\n  FAR="); uart_hex64(tf->far_el1);
    uart_puts("  SPSR="); uart_hex64(tf->spsr_el1);
    uart_puts("\r\n");
}

static void halt(void)
{
    uart_puts("  kernel halted.\r\n");
    for (;;)
        __asm__ volatile("wfe");
}

void arm64_exception(struct trapframe *tf, unsigned long kind)
{
    uint32_t ec = (uint32_t)((tf->esr_el1 >> 26) & 0x3F);

    /* Self-test hook: a deliberately-trapped undefined instruction is
     * skipped and execution resumes (see exceptions_selftest). */
    if (g_expect_trap && ec == EC_UNKNOWN) {
        g_trapped_ec  = ec;
        g_expect_trap = 0;
        tf->elr_el1  += 4;      /* step over the faulting instruction */
        return;
    }

    /* IRQ / FIQ / SError have no useful EC; identify them by vector slot. */
    unsigned type = kind & 3;   /* 0=sync 1=IRQ 2=FIQ 3=SError */
    if (type == 1) { uart_puts("\r\n[exception] unexpected IRQ\r\n");    dump(tf, ec, kind); halt(); }
    if (type == 2) { uart_puts("\r\n[exception] unexpected FIQ\r\n");    dump(tf, ec, kind); halt(); }
    if (type == 3) { uart_puts("\r\n[exception] SError\r\n");           dump(tf, ec, kind); halt(); }

    int from_el0 = (kind >= 8);

    switch (arm64_ec_class(ec)) {
    case EC_CLASS_SVC:
        arm64_handle_svc(tf);   /* recoverable: returns to EL0 */
        return;

    case EC_CLASS_DATA_ABORT:
    case EC_CLASS_INSTR_ABORT:
        uart_puts("\r\n[exception] memory fault\r\n");
        dump(tf, ec, kind);
        if (from_el0) {
            uart_puts("  -> terminating EL0 process\r\n");
            arm64_user_fault(tf);   /* strong version does not return */
        }
        halt();
        return;

    case EC_CLASS_UNKNOWN:
    default:
        uart_puts("\r\n[exception] unhandled synchronous exception\r\n");
        dump(tf, ec, kind);
        if (from_el0) {
            uart_puts("  -> terminating EL0 process\r\n");
            arm64_user_fault(tf);   /* strong version does not return */
        }
        halt();
        return;
    }
}

/* Weak default SVC handler; issue #13 replaces this with the syscall ABI.
 * The AArch64 syscall-number convention used here is x8 = number. */
__attribute__((weak)) void arm64_handle_svc(struct trapframe *tf)
{
    uart_printf("  svc    : syscall #%u from EL0 (no handler yet)\r\n",
                (unsigned)tf->x[8]);
    tf->x[0] = 0;
}

/* Weak default EL0-fault hook; issue #13 provides a strong version that
 * terminates the faulting process via kernel_resume().  The weak version
 * returns, so the caller falls through to halt(). */
__attribute__((weak)) void arm64_user_fault(struct trapframe *tf)
{
    (void)tf;
}

void exceptions_selftest(void)
{
    uart_puts("=== exception self-test (issue #12) ===\r\n");

    /* Trigger an undefined instruction (UDF #0 == .inst 0x00000000) and let
     * the handler recover, proving the full vector -> save -> dispatch ->
     * ESR decode -> ERET path works without bricking the kernel. */
    g_expect_trap = 1;
    __asm__ volatile(".inst 0x00000000");

    int ok = (g_expect_trap == 0) && (g_trapped_ec == EC_UNKNOWN);
    uart_printf("exc   : undefined-instruction trap ... %s (EC=0x%x)\r\n",
                ok ? "caught+recovered" : "MISSED", (unsigned)g_trapped_ec);
    uart_puts("=== exception self-test complete ===\r\n\r\n");
}

#endif /* !HOSTTEST */
