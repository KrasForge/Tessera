/* arch/arm64/exceptions.h — AArch64 exception handling (Issue #12, M2)
 *
 * The ARM replacement for the x86 IDT (kernel/idt.c, interrupt_stubs.asm):
 * the CPU vectors all exceptions through a 2 KiB-aligned table pointed to by
 * VBAR_EL1.  arch/arm64/vectors.S saves a full register frame and calls
 * arm64_exception(), which decodes ESR_EL1 and dispatches.
 */

#ifndef ARM64_EXCEPTIONS_H
#define ARM64_EXCEPTIONS_H

#include <stdint.h>

/* Register frame saved by vectors.S on every exception entry.  The field
 * order and offsets MUST match the stp/ldp sequence in vectors.S. */
struct trapframe {
    uint64_t x[31];     /* x0 .. x30                      (offset 0x000) */
    uint64_t sp_el0;    /* user stack pointer             (offset 0x0F8) */
    uint64_t elr_el1;   /* exception return address       (offset 0x100) */
    uint64_t spsr_el1;  /* saved program status           (offset 0x108) */
    uint64_t esr_el1;   /* exception syndrome             (offset 0x110) */
    uint64_t far_el1;   /* faulting virtual address       (offset 0x118) */
};

/* Exception-class (ESR_EL1.EC) values we decode specifically. */
#define EC_UNKNOWN       0x00   /* undefined / unallocated instruction */
#define EC_SVC_A64       0x15   /* SVC from AArch64 (syscall)          */
#define EC_INSTR_ABORT_L 0x20   /* instruction abort, lower EL (EL0)   */
#define EC_INSTR_ABORT_S 0x21   /* instruction abort, same EL  (EL1)   */
#define EC_DATA_ABORT_L  0x24   /* data abort, lower EL (EL0)          */
#define EC_DATA_ABORT_S  0x25   /* data abort, same EL  (EL1)          */
#define EC_FP_ACCESS     0x07   /* Advanced SIMD / FP access trap      */

/* Coarse classification of an exception class. */
typedef enum {
    EC_CLASS_UNKNOWN = 0,   /* undefined instruction / unallocated */
    EC_CLASS_SVC,           /* supervisor call (syscall)           */
    EC_CLASS_INSTR_ABORT,   /* instruction fetch fault             */
    EC_CLASS_DATA_ABORT,    /* data access fault                   */
    EC_CLASS_OTHER,         /* anything else                       */
} ec_class_t;

/* Pure decoders (also unit-tested on the host). */
const char *arm64_ec_name(uint32_t ec);
ec_class_t  arm64_ec_class(uint32_t ec);

/* Abort-syndrome decoders (valid for data/instruction abort EC values).
 * WnR (write-not-read) is bit 6 of a data-abort ISS; the 6-bit fault status
 * code (DFSC/IFSC) is ESR[5:0]. */
int      arm64_abort_is_write(uint64_t esr);
uint32_t arm64_abort_fsc(uint64_t esr);

/* Install the vector table in VBAR_EL1.  Call before any user code runs. */
void exceptions_init(void);

/* Top-level dispatcher, called from vectors.S.  `kind` is the vector-table
 * slot index (0..15). */
void arm64_exception(struct trapframe *tf, unsigned long kind);

/* SVC handler (weak default here; issue #13 provides the real syscall ABI). */
void arm64_handle_svc(struct trapframe *tf);

/* On-target demonstration: trap a recoverable undefined instruction. */
void exceptions_selftest(void);

#endif /* ARM64_EXCEPTIONS_H */
