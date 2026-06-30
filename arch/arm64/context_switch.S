/* arch/arm64/context_switch.S - AArch64 context switching (Issue #15, M2)
 *
 * Replaces the x86 TSS-based switch in kernel/context_switch.asm.  Provides:
 *
 *   cpu_switch(prev, next)  - the classic kernel-thread switch: save the
 *                             callee-saved set (x19-x30) and SP of the
 *                             outgoing thread, restore the incoming one's,
 *                             and return into it.
 *
 *   switch_mm(ttbr0)        - switch the EL0 address space.  ASID and table
 *                             base are changed together; to avoid the CPU
 *                             speculatively allocating TLB entries under the
 *                             old ASID, a reserved ASID (0, global-only) is
 *                             installed first, then the new ASID+base.
 *
 *   enter_context(tf)       - load a full register frame (x0-x30, SP_EL0,
 *                             ELR_EL1, SPSR_EL1) and ERET to it.  Used by the
 *                             scheduler to start or resume an EL0 task.  The
 *                             outgoing task's frame is saved by the exception
 *                             vector (vectors.S), so a cooperative switch is
 *                             "swap the frame + switch_mm".
 *
 *   sched_first(tf)         - save the kernel context (so the scheduler can
 *                             be unwound back to when the run queue empties)
 *                             then enter_context(tf).
 *
 *   fpu_save / fpu_restore  - save/restore the NEON register file (q0-q31 +
 *                             FPSR/FPCR) for lazy FP context switching.
 *   fpu_enable / fpu_disable- toggle CPACR_EL1.FPEN (trap-on-use).
 */

    .section .text

/* ---- kernel-thread switch ------------------------------------------- */
/* struct cpu_context { uint64_t x19..x30[12]; uint64_t sp; }  (104 bytes) */
    .global cpu_switch
cpu_switch:                     /* x0 = prev, x1 = next */
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]

    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x2, [x1, #96]
    mov     sp, x2
    ret                         /* returns into next's restored x30 (lr) */

/* ---- address-space switch ------------------------------------------- */
    .global switch_mm
switch_mm:                      /* x0 = TTBR0 value (BADDR | ASID<<48) */
    /* 1. Install the reserved ASID (0) with the *old* base to quiesce, so
     *    no new entries are allocated under the outgoing ASID. */
    mrs     x1, ttbr0_el1
    and     x1, x1, #0x0000FFFFFFFFFFFF      /* clear ASID[63:48] -> ASID 0 */
    msr     ttbr0_el1, x1
    isb
    /* 2. Install the new base + new ASID together (single atomic write). */
    msr     ttbr0_el1, x0
    isb
    ret

/* ---- enter / resume an EL0 task ------------------------------------- */
/* Mirrors kernel_exit in vectors.S but reads the frame from x0 (not SP) and
 * does not touch SP_EL1 (the frame lives in the task PCB, not on the stack). */
    .global enter_context
enter_context:                  /* x0 = struct trapframe * */
    ldp     x10, x11, [x0, #16 * 16]
    msr     elr_el1,  x10
    msr     spsr_el1, x11
    ldp     x30, x9,  [x0, #16 * 15]
    msr     sp_el0,   x9
    ldp     x2,  x3,  [x0, #16 * 1]
    ldp     x4,  x5,  [x0, #16 * 2]
    ldp     x6,  x7,  [x0, #16 * 3]
    ldp     x8,  x9,  [x0, #16 * 4]
    ldp     x10, x11, [x0, #16 * 5]
    ldp     x12, x13, [x0, #16 * 6]
    ldp     x14, x15, [x0, #16 * 7]
    ldp     x16, x17, [x0, #16 * 8]
    ldp     x18, x19, [x0, #16 * 9]
    ldp     x20, x21, [x0, #16 * 10]
    ldp     x22, x23, [x0, #16 * 11]
    ldp     x24, x25, [x0, #16 * 12]
    ldp     x26, x27, [x0, #16 * 13]
    ldp     x28, x29, [x0, #16 * 14]
    ldr     x1, [x0, #8]        /* user x1 */
    ldr     x0, [x0, #0]        /* user x0 (frame ptr consumed last) */
    eret

/* ---- scheduler entry ------------------------------------------------ */
    .global sched_first
sched_first:                    /* x0 = first task's trapframe */
    adrp    x9, g_kresume
    add     x9, x9, :lo12:g_kresume
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    mov     x10, sp
    str     x10, [x9, #96]
    b       enter_context       /* x0 still = trapframe */

/* ---- lazy FPU/NEON --------------------------------------------------- */
/* Save/restore area layout: q0..q31 (512 bytes) then FPSR, FPCR (32-bit). */
    .global fpu_save
fpu_save:                       /* x0 = area */
    stp     q0,  q1,  [x0, #16 * 0]
    stp     q2,  q3,  [x0, #16 * 2]
    stp     q4,  q5,  [x0, #16 * 4]
    stp     q6,  q7,  [x0, #16 * 6]
    stp     q8,  q9,  [x0, #16 * 8]
    stp     q10, q11, [x0, #16 * 10]
    stp     q12, q13, [x0, #16 * 12]
    stp     q14, q15, [x0, #16 * 14]
    stp     q16, q17, [x0, #16 * 16]
    stp     q18, q19, [x0, #16 * 18]
    stp     q20, q21, [x0, #16 * 20]
    stp     q22, q23, [x0, #16 * 22]
    stp     q24, q25, [x0, #16 * 24]
    stp     q26, q27, [x0, #16 * 26]
    stp     q28, q29, [x0, #16 * 28]
    stp     q30, q31, [x0, #16 * 30]
    mrs     x1, fpsr
    str     w1, [x0, #512]
    mrs     x1, fpcr
    str     w1, [x0, #516]
    ret

    .global fpu_restore
fpu_restore:                    /* x0 = area */
    ldp     q0,  q1,  [x0, #16 * 0]
    ldp     q2,  q3,  [x0, #16 * 2]
    ldp     q4,  q5,  [x0, #16 * 4]
    ldp     q6,  q7,  [x0, #16 * 6]
    ldp     q8,  q9,  [x0, #16 * 8]
    ldp     q10, q11, [x0, #16 * 10]
    ldp     q12, q13, [x0, #16 * 12]
    ldp     q14, q15, [x0, #16 * 14]
    ldp     q16, q17, [x0, #16 * 16]
    ldp     q18, q19, [x0, #16 * 18]
    ldp     q20, q21, [x0, #16 * 20]
    ldp     q22, q23, [x0, #16 * 22]
    ldp     q24, q25, [x0, #16 * 24]
    ldp     q26, q27, [x0, #16 * 26]
    ldp     q28, q29, [x0, #16 * 28]
    ldp     q30, q31, [x0, #16 * 30]
    ldr     w1, [x0, #512]
    msr     fpsr, x1
    ldr     w1, [x0, #516]
    msr     fpcr, x1
    ret

    .global fpu_enable
fpu_enable:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)  /* FPEN = 0b11: no trapping */
    msr     cpacr_el1, x0
    isb
    ret

    .global fpu_disable
fpu_disable:
    mrs     x0, cpacr_el1
    bic     x0, x0, #(3 << 20)  /* FPEN = 0b00: trap FP/SIMD at EL0 and EL1 */
    msr     cpacr_el1, x0
    isb
    ret
