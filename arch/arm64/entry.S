/* arch/arm64/entry.S — EL0 entry / exit (Issue #13, M2)
 *
 * run_user() drops to EL0 to run a process; kernel_resume() unwinds back to
 * run_user()'s caller when the process calls sys_exit or faults.  Together
 * they form a one-shot "switch to user, run until exit" primitive (the
 * register save/restore is a setjmp/longjmp over the eret boundary).
 *
 * Replaces the x86 ring-3 entry in kernel/user_mode.asm.
 */

#include "el0_context.h"

    .section .text

/* long run_user(uint64_t entry x0, uint64_t user_sp x1, uint64_t ttbr0 x2,
 *               uint64_t arg0 x3)  -- arg0 is placed in the user's x0. */
    .global run_user
run_user:
    adrp    x9, g_kresume
    add     x9, x9, :lo12:g_kresume
    mrs     x11, mpidr_el1
    and     x11, x11, #(EL0_CONTEXT_CPUS - 1)
    add     x9, x9, x11, lsl #7     /* 128-byte slot owned by this core */

    /* Save the kernel callee-saved context so kernel_resume() can return
     * here as if run_user() returned normally. */
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    mov     x10, sp
    str     x10, [x9, #96]

    /* Install the process address space (ASID-tagged).  Harmless when the
     * MMU is off (writes are ignored for translation). */
    msr     ttbr0_el1, x2
    dsb     ish
    /* A local address-space switch does not change another core's page
     * tables. Keep this invalidation local; vmem updates retain their IS
     * shootdowns. Broadcasting here stalls unrelated audio/reporting cores. */
    tlbi    vmalle1
    dsb     ish
    isb

    /* Enter EL0: ELR = entry, SPSR = g_user_spsr (EL0t with DAIF masked by
     * default; budget enforcement (issue #78) opens the IRQ bit so a plugin
     * can be preempted at its budget boundary), SP_EL0 = stack, and pass
     * arg0 in the user's x0. */
    msr     elr_el1, x0
    adrp    x10, g_user_spsr
    add     x10, x10, :lo12:g_user_spsr
    ldr     x10, [x10, x11, lsl #3]
    msr     spsr_el1, x10
    msr     sp_el0, x1
    mov     x0, x3
    eret

/* void kernel_resume(long code x0) — restore kernel context; run_user()
 * returns `code`.  Never returns to the caller of kernel_resume(). */
    .global kernel_resume
kernel_resume:
    adrp    x9, g_kresume
    add     x9, x9, :lo12:g_kresume
    mrs     x11, mpidr_el1
    and     x11, x11, #(EL0_CONTEXT_CPUS - 1)
    add     x9, x9, x11, lsl #7     /* 128-byte slot owned by this core */

    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldr     x10, [x9, #96]
    mov     sp, x10
    ret                         /* x0 (code) is the return value of run_user */

/* Private kernel helpers: save/restore FP state around process_run. */
    .global el0_fp_save
el0_fp_save:
    stp q0, q1, [x0, #0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    mrs x2, fpsr
    /* The signed scaled STP/LDP immediate ends at 504 for X registers. */
    str x1, [x0, #512]
    str x2, [x0, #520]
    ret
    .global el0_fp_restore
el0_fp_restore:
    ldp q0, q1, [x0, #0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldr x1, [x0, #512]
    ldr x2, [x0, #520]
    msr fpcr, x1
    msr fpsr, x2
    ret

    .section .bss
    .align  4
    .global g_kresume
g_kresume:
    .skip   (EL0_CONTEXT_CPUS * EL0_RESUME_BYTES)

/* SPSR used for the EL0 entry.  0x3C0 = EL0t, DAIF masked (the default);
 * budget_arm() (issue #78) switches to 0x340 (IRQ open) around a budgeted
 * plugin call so its timer can preempt the plugin, and budget_disarm()
 * restores the default. */
    .section .data
    .align  3
    .global g_user_spsr
g_user_spsr:
    .rept EL0_CONTEXT_CPUS
    .quad   0x3C0
    .endr
