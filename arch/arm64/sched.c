/* arch/arm64/sched.c - cooperative round-robin scheduler + lazy FPU
 *                       (Issue #15, M2)
 *
 * Cooperative switching works by mutating the exception register frame in
 * place: when a task traps into the kernel (sys_yield, sys_exit, or a fault),
 * vectors.S has already saved its full EL0 frame on the kernel stack.  The
 * scheduler copies that frame into the task's PCB, switches the address space
 * with switch_mm(), copies the next task's saved frame back over the on-stack
 * frame, and returns; the vector's restore/ERET then resumes the next task.
 *
 * FPU/NEON state is switched lazily: FP access is trapped (CPACR_EL1.FPEN),
 * and only when a task actually touches FP do we save the previous owner's
 * NEON registers and restore this task's.  A task that never uses FP pays
 * nothing.
 */

#include "sched.h"
#include "process.h"
#include "exceptions.h"
#include "usermode.h"
#include "uart_pl011.h"
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
static sched_task_t *g_fpu_owner;
static uint64_t      g_kernel_ttbr;

int sched_active(void) { return g_running; }

void sched_init(void)
{
    g_ntask = 0;
    g_cur = 0;
    g_running = 0;
    g_fpu_owner = (sched_task_t *)0;
    for (int i = 0; i < MAX_TASKS; i++) {
        g_task[i].p = (process_t *)0;
        g_task[i].live = 0;
    }
}

static void init_frame(struct trapframe *tf, uint64_t entry, uint64_t sp,
                       uint64_t arg)
{
    memset(tf, 0, sizeof(*tf));
    tf->elr_el1  = entry;
    tf->spsr_el1 = 0x3C0;     /* EL0t, DAIF masked */
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
    memset(t->fpu, 0, FPU_AREA);
    init_frame(&t->ctx, entry, user_sp, arg0);
    return g_ntask++;
}

/* Index of the next live task after `from` (round-robin), or -1 if none. */
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

void sched_yield(struct trapframe *tf)
{
    memcpy(&g_task[g_cur].ctx, tf, sizeof(*tf));   /* save current task */
    int nx = next_live(g_cur);
    if (nx < 0 || nx == g_cur)
        return;                                    /* nobody else to run */
    resume_task(nx, tf);
}

static void retire(int killed, struct trapframe *tf, long code)
{
    g_task[g_cur].live = 0;
    g_task[g_cur].p->exit_code = code;
    g_task[g_cur].p->state = killed ? PROC_KILLED : PROC_ZOMBIE;
    if (g_fpu_owner == &g_task[g_cur])
        g_fpu_owner = (sched_task_t *)0;

    int nx = next_live(g_cur);
    if (nx < 0)
        kernel_resume(code);       /* run queue empty: unwind to sched_run */
    resume_task(nx, tf);
}

void sched_exit(struct trapframe *tf, long code) { retire(0, tf, code); }
void sched_kill(struct trapframe *tf)            { retire(1, tf, -1); }

long sched_run(void)
{
    if (g_ntask == 0)
        return 0;

    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(g_kernel_ttbr));

    g_running   = 1;
    g_cur       = 0;
    g_fpu_owner = (sched_task_t *)0;
    fpu_disable();
    switch_mm(g_task[0].p->ttbr0);
    process_set_current(g_task[0].p);

    sched_first(&g_task[0].ctx);   /* ERETs into task 0; returns here at end */

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
