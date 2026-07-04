/* arch/arm64/sched.c - cooperative + preemptive RT scheduler + lazy FPU
 *                       (Issue #15, M2; preemption + priorities, Issue #20, M4)
 *
 * Switching works by mutating the exception register frame in place: when a
 * task enters the kernel (sys_yield/sys_exit/fault in cooperative mode, or a
 * timer IRQ in preemptive mode), vectors.S has already saved its full EL0
 * frame on the kernel stack.  The scheduler copies that frame into the task's
 * PCB, switches the address space with switch_mm(), copies the next task's
 * saved frame back over the on-stack frame, and returns; the vector's
 * restore/ERET then resumes the next task.  The same mechanism therefore
 * serves both voluntary yields and timer-driven preemption.
 *
 * Scheduling policy (which task runs next) lives in the pure, host-tested
 * run queue (runqueue.c): three priority bands with round-robin within a band.
 * This is the ARM port of kernel/scheduler.c; the x86 cli/sti critical section
 * around run-queue mutation is replaced here by DAIF (IRQ) masking.
 *
 * FPU/NEON state is switched lazily: FP access is trapped (CPACR_EL1.FPEN),
 * and only when a task actually touches FP do we save the previous owner's
 * NEON registers and restore this task's.  A task that never uses FP pays
 * nothing.
 */

#include "sched.h"
#include "runqueue.h"
#include "process.h"
#include "exceptions.h"
#include "usermode.h"
#include "uart_pl011.h"
#include "denorm.h"

#include <stdint.h>
#include <stddef.h>

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

/* context_switch.S */
void switch_mm(uint64_t ttbr0);
void enter_context(struct trapframe *tf);
void sched_first(struct trapframe *tf);
void fpu_save(void *area);
void fpu_restore(void *area);
void fpu_enable(void);
void fpu_disable(void);

#define MAX_TASKS  8
#define FPU_AREA   528   /* q0..q31 (512) + FPSR/FPCR, padded */
#define FPU_FPCR_OFF 516 /* FPCR word within the FPU save area (see context_switch.S) */

/* Zero a task's FPU save area and seed its FPCR with flush-to-zero on, so the
 * task inherits denormal protection the moment its FP context is restored
 * (Theme H, issue #130). */
static void fpu_area_init(uint8_t *fpu)
{
    memset(fpu, 0, FPU_AREA);
    uint32_t fpcr = denorm_fpcr_default();
    memcpy(fpu + FPU_FPCR_OFF, &fpcr, sizeof fpcr);
}

typedef struct {
    process_t        *p;
    struct trapframe  ctx;                 /* saved EL0 frame when not running */
    uint8_t           fpu[FPU_AREA] __attribute__((aligned(16)));
    int               live;
} sched_task_t;

static sched_task_t  g_task[MAX_TASKS];
static int           g_ntask;
static int           g_cur;
static int           g_running;
static int           g_preempt;        /* priority/preemptive mode active */
static sched_task_t *g_fpu_owner;
static uint64_t      g_kernel_ttbr;

int sched_active(void) { return g_running; }

/* DAIF (IRQ) critical section: the ARM replacement for the x86 cli/sti pair
 * that guarded the run-queue mutation in kernel/scheduler.c.  Mask IRQ around
 * run-queue changes made outside exception context (e.g. sched_add_prio); on
 * the IRQ/SVC paths the CPU already masks IRQ on exception entry. */
static inline uint64_t irq_save(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");   /* set I (mask IRQ) */
    return daif;
}

static inline void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif));
}

/* Weak instrumentation hook (strong version provided by test harnesses). */
__attribute__((weak)) void sched_tick_observer(int task_idx) { (void)task_idx; }

void sched_init(void)
{
    g_ntask = 0;
    g_cur = 0;
    g_running = 0;
    g_preempt = 0;
    g_fpu_owner = (sched_task_t *)0;
    for (int i = 0; i < MAX_TASKS; i++) {
        g_task[i].p = (process_t *)0;
        g_task[i].live = 0;
    }
    rq_init();
}

static void init_frame(struct trapframe *tf, uint64_t entry, uint64_t sp,
                       uint64_t arg, int irq_on)
{
    memset(tf, 0, sizeof(*tf));
    tf->elr_el1  = entry;
    /* EL0t.  DAIF: mask FIQ/Abort/Debug; unmask IRQ (clear bit 7) when the
     * task is preemptible so the timer can interrupt it at EL0. */
    tf->spsr_el1 = irq_on ? 0x340 : 0x3C0;
    tf->sp_el0   = sp;
    tf->x[0]     = arg;
}

int sched_add(process_t *p, uint64_t entry, uint64_t user_sp, uint64_t arg0)
{
    if (g_ntask >= MAX_TASKS)
        return -1;
    sched_task_t *t = &g_task[g_ntask];
    t->p = p;
    t->live = 1;
    fpu_area_init(t->fpu);
    init_frame(&t->ctx, entry, user_sp, arg0, /*irq_on=*/0);
    return g_ntask++;
}

int sched_add_prio(process_t *p, uint64_t entry, uint64_t user_sp,
                   uint64_t arg0, int prio, uint32_t quantum)
{
    if (g_ntask >= MAX_TASKS)
        return -1;
    uint64_t flags = irq_save();
    int idx = g_ntask;
    sched_task_t *t = &g_task[idx];
    t->p = p;
    t->live = 1;
    fpu_area_init(t->fpu);
    init_frame(&t->ctx, entry, user_sp, arg0, /*irq_on=*/1);
    rq_add(idx, prio, quantum);
    g_preempt = 1;
    g_ntask++;
    irq_restore(flags);
    return idx;
}

/* Index of the next live task after `from` (cooperative round-robin), or -1. */
static int next_live(int from)
{
    for (int k = 1; k <= g_ntask; k++) {
        int i = (from + k) % g_ntask;
        if (g_task[i].live)
            return i;
    }
    return -1;
}

/* Make task `idx` current: switch address space, mark FP trap-on-use, and
 * copy its saved frame over the on-stack exception frame so the vector ERETs
 * into it. */
static void resume_task(int idx, struct trapframe *tf)
{
    g_cur = idx;
    fpu_disable();                 /* the new task re-traps on first FP use */
    switch_mm(g_task[idx].p->ttbr0);
    process_set_current(g_task[idx].p);
    memcpy(tf, &g_task[idx].ctx, sizeof(*tf));
}

/* Pick the successor of the current task.  In preemptive mode the choice is
 * priority-aware (highest band, round-robin within it); cooperatively it is a
 * plain round-robin over live tasks.  Returns -1 if nobody else can run. */
static int pick_next(void)
{
    if (g_preempt) {
        int nx = rq_pick();
        return nx;                 /* RQ_NONE (-1) when the run queue is empty */
    }
    return next_live(g_cur);
}

void sched_yield(struct trapframe *tf)
{
    memcpy(&g_task[g_cur].ctx, tf, sizeof(*tf));   /* save current task */

    if (g_preempt)
        rq_commit(g_cur);          /* rotate yielding task to the band tail */

    int nx = pick_next();
    if (nx < 0 || nx == g_cur)
        return;                    /* nobody else to run */

    if (g_preempt)
        rq_commit(nx);
    resume_task(nx, tf);
}

static void retire(int killed, struct trapframe *tf, long code)
{
    g_task[g_cur].live = 0;
    g_task[g_cur].p->exit_code = code;
    g_task[g_cur].p->state = killed ? PROC_KILLED : PROC_ZOMBIE;
    if (g_fpu_owner == &g_task[g_cur])
        g_fpu_owner = (sched_task_t *)0;
    if (g_preempt)
        rq_remove(g_cur);

    int nx = pick_next();
    if (nx < 0)
        kernel_resume(code);       /* run queue empty: unwind to sched_run */
    if (g_preempt)
        rq_commit(nx);
    resume_task(nx, tf);
}

void sched_exit(struct trapframe *tf, long code) { retire(0, tf, code); }
void sched_kill(struct trapframe *tf)            { retire(1, tf, -1); }

/* Timer-tick preemption point (issue #20): runs in IRQ context with the
 * interrupted task's frame.  Decides via the run queue whether to keep the
 * current task or switch, and performs the frame swap when switching. */
void scheduler_tick(struct trapframe *tf)
{
    if (!g_running || !g_preempt)
        return;

    int nx = rq_tick();
    if (nx != RQ_NONE && nx != g_cur) {
        memcpy(&g_task[g_cur].ctx, tf, sizeof(*tf));   /* save preempted task */
        rq_commit(nx);
        resume_task(nx, tf);                           /* install successor   */
    }
    sched_tick_observer(g_cur);
}

long sched_run(void)
{
    if (g_ntask == 0)
        return 0;

    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(g_kernel_ttbr));

    int first = 0;
    if (g_preempt) {
        first = rq_pick();         /* start with the highest-priority task */
        if (first < 0)
            return 0;
        rq_commit(first);
    }

    g_running   = 1;
    g_cur       = first;
    g_fpu_owner = (sched_task_t *)0;
    fpu_disable();
    switch_mm(g_task[first].p->ttbr0);
    process_set_current(g_task[first].p);

    sched_first(&g_task[first].ctx);  /* ERETs into the task; returns at end */

    /* Back in the kernel: restore the kernel address space. */
    __asm__ volatile("msr ttbr0_el1, %0; isb" :: "r"(g_kernel_ttbr));
    process_set_current((process_t *)0);
    g_running = 0;
    return 0;
}

/* FP-access trap (CPACR_EL1.FPEN): enable FP, then lazily switch the NEON
 * register file if a different task now owns the FPU. */
void arm64_fpu_trap(struct trapframe *tf)
{
    (void)tf;
    fpu_enable();
    if (!g_running)
        return;                    /* single-process mode: just allow FP */

    sched_task_t *t = &g_task[g_cur];
    if (g_fpu_owner != t) {
        if (g_fpu_owner)
            fpu_save(g_fpu_owner->fpu);
        fpu_restore(t->fpu);
        g_fpu_owner = t;
    }
}
