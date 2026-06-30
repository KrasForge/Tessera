/* arch/arm64/vectors.S — AArch64 exception vector table (Issue #12, M2)
 *
 * Replaces the x86 IDT + interrupt stubs.  VBAR_EL1 points at `vectors`,
 * which is 2 KiB aligned and holds 16 entries (4 exception types — sync,
 * IRQ, FIQ, SError — for each of 4 sources).  Each entry branches to a
 * handler that saves a full register frame, calls the C dispatcher with the
 * slot index, then restores and ERETs.
 *
 * Frame layout (must match struct trapframe in exceptions.h):
 *   0x000  x0..x30        (16*15 = 0xF0 + the x30/sp_el0 pair)
 *   0x0F0  x30, sp_el0
 *   0x100  elr_el1, spsr_el1
 *   0x110  esr_el1, far_el1
 *   0x120  = FRAME_SIZE
 */

#define FRAME_SIZE 0x120

/* ---- save / restore -------------------------------------------------- */

.macro kernel_entry
    sub     sp, sp, #FRAME_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x9,  sp_el0
    stp     x30, x9,  [sp, #16 * 15]
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    stp     x10, x11, [sp, #16 * 16]
    mrs     x12, esr_el1
    mrs     x13, far_el1
    stp     x12, x13, [sp, #16 * 17]
.endm

.macro kernel_exit
    ldp     x10, x11, [sp, #16 * 16]
    msr     elr_el1,  x10
    msr     spsr_el1, x11
    ldp     x30, x9,  [sp, #16 * 15]
    msr     sp_el0,   x9
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #FRAME_SIZE
    eret
.endm

.macro HANDLER name, kind
\name:
    kernel_entry
    mov     x0, sp
    mov     x1, #\kind
    bl      arm64_exception
    b       ret_from_exception
.endm

/* ---- the table ------------------------------------------------------- */

.macro VEC handler
    .balign 0x80
    b       \handler
.endm

    .section .text
    .balign 0x800
    .global vectors
vectors:
    VEC el1t_sync          /*  0: current EL, SP_EL0, synchronous */
    VEC el1t_irq           /*  1 */
    VEC el1t_fiq           /*  2 */
    VEC el1t_serror        /*  3 */
    VEC el1h_sync          /*  4: current EL, SP_ELx, synchronous */
    VEC el1h_irq           /*  5 */
    VEC el1h_fiq           /*  6 */
    VEC el1h_serror        /*  7 */
    VEC el0_sync           /*  8: lower EL (AArch64), synchronous  */
    VEC el0_irq            /*  9 */
    VEC el0_fiq            /* 10 */
    VEC el0_serror         /* 11 */
    VEC el0_32_sync        /* 12: lower EL (AArch32), synchronous  */
    VEC el0_32_irq         /* 13 */
    VEC el0_32_fiq         /* 14 */
    VEC el0_32_serror      /* 15 */

/* ---- handlers -------------------------------------------------------- */

    HANDLER el1t_sync,     0
    HANDLER el1t_irq,      1
    HANDLER el1t_fiq,      2
    HANDLER el1t_serror,   3
    HANDLER el1h_sync,     4
    HANDLER el1h_irq,      5
    HANDLER el1h_fiq,      6
    HANDLER el1h_serror,   7
    HANDLER el0_sync,      8
    HANDLER el0_irq,       9
    HANDLER el0_fiq,       10
    HANDLER el0_serror,    11
    HANDLER el0_32_sync,   12
    HANDLER el0_32_irq,    13
    HANDLER el0_32_fiq,    14
    HANDLER el0_32_serror, 15

ret_from_exception:
    kernel_exit
