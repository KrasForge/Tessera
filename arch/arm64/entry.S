/* arch/arm64/entry.S — EL0 entry / exit (Issue #13, M2)
 *
 * run_user() drops to EL0 to run a process; kernel_resume() unwinds back to
 * run_user()'s caller when the process calls sys_exit or faults.  Together
 * they form a one-shot "switch to user, run until exit" primitive (the
 * register save/restore is a setjmp/longjmp over the eret boundary).
 *
 * Replaces the x86 ring-3 entry in kernel/user_mode.asm.
 */

    .section .text

/* long run_user(uint64_t entry x0, uint64_t user_sp x1, uint64_t ttbr0 x2,
 *               uint64_t arg0 x3)  -- arg0 is placed in the user's x0. */
    .global run_user
run_user:
    adrp    x9, g_kresume
    add     x9, x9, :lo12:g_kresume

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
    tlbi    vmalle1is
    dsb     ish
    isb

    /* Enter EL0: ELR = entry, SPSR = EL0t with DAIF masked, SP_EL0 = stack,
     * and pass arg0 in the user's x0. */
    msr     elr_el1, x0
    mov     x10, #0x3C0
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

    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldr     x10, [x9, #96]
    mov     sp, x10
    ret                         /* x0 (code) is the return value of run_user */

    .section .bss
    .align  4
g_kresume:
    .skip   112                 /* x19..x30 (12) + sp (1), padded to 16 */
