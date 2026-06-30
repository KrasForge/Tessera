/* tests/arm64/virt/preempt_main.c - preemptive RT scheduler on QEMU 'virt'
 * (Issue #20).
 *
 * This is the real-hardware proof of preemption.  It brings up the full stack
 * with the MMU on (pmm + mmu + process + exceptions + scheduler) AND the GICv2
 * + generic timer (issue #19), then runs four EL0 tasks that are pure CPU-bound
 * busy loops which never make a syscall:
 *
 *   RT    - REALTIME priority, short loop
 *   NA,NB - NORMAL priority, long loops
 *   IDLE  - IDLE priority, short loop
 *
 * Because the busy loops never yield, they can only leave the CPU by being
 * *preempted* by the 1 kHz timer IRQ.  A per-tick observer records which task
 * is running, and afterwards the harness checks the three acceptance criteria:
 *
 *   1. The REALTIME task runs to completion before any NORMAL task starts
 *      (priority preemption: RT starves the NORMAL tasks while runnable).
 *   2. The two NORMAL tasks interleave in time (preemptive round-robin: with a
 *      cooperative scheduler the first would run to completion before the
 *      second ever started).
 *   3. The IDLE task runs only after both NORMAL tasks have finished.
 *
 * Built with the virt MMU map (RAM 0x40000000, MMIO 0x08000000) and the virt
 * GIC bases (0x08000000 / 0x08010000).
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "sched.h"
#include "runqueue.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void  uart_virt_init(void);
void  exceptions_init(void);
void *memcpy(void *, const void *, size_t);
void  fpu_disable(void);

extern char user_busy[], user_busy_end[];

#define USER_CODE_VA   USER_VA_BASE
#define USER_STACK_VA  (USER_VA_BASE + 0x10000)
#define USER_SP        (USER_STACK_VA + PAGE_SIZE)

/* CPU-bound iteration counts.  Sized so every task spans many 1 ms ticks (so
 * preemption and round-robin are clearly observable) while the whole run stays
 * well under the QEMU timeout. */
#define RT_ITERS    8000000UL
#define N_ITERS     30000000UL
#define IDLE_ITERS  4000000UL

/* ---- per-tick observer -------------------------------------------------- */

#define NTASK 4
static volatile uint64_t g_now;                 /* monotonic tick index     */
static volatile int      g_count[NTASK];        /* ticks each task ran      */
static volatile int64_t  g_first[NTASK];        /* first tick seen running  */
static volatile int64_t  g_last[NTASK];         /* last tick seen running   */

/* Strong override of the scheduler's weak hook: called once per timer tick
 * with the id of the task that will run next. */
void sched_tick_observer(int idx)
{
    uint64_t t = g_now++;
    if (idx < 0 || idx >= NTASK)
        return;
    if (g_first[idx] < 0)
        g_first[idx] = (int64_t)t;
    g_last[idx] = (int64_t)t;
    g_count[idx]++;
}

/* Create an EL0 process running user_busy(iters) at priority `prio`. */
static process_t *make_busy(const char *name, uint64_t iters, int prio)
{
    process_t *p = process_create(name);
    size_t len = (size_t)(user_busy_end - user_busy);

    uintptr_t code_pa = phys_alloc_page();
    memcpy((void *)code_pa, user_busy, len);
    process_map(p, code_pa, USER_CODE_VA, PAGE_SIZE, VMM_READ | VMM_EXEC);

    uintptr_t stack_pa = phys_alloc_page();
    process_map(p, stack_pa, USER_STACK_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);

    sched_add_prio(p, USER_CODE_VA, USER_SP, iters, prio, 0);
    return p;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt preemptive RT scheduler (issue #20) ===\r\n");

    for (int i = 0; i < NTASK; i++) {
        g_first[i] = -1;
        g_last[i]  = -1;
        g_count[i] = 0;
    }

    pmm_init();
    mmu_init();
    exceptions_init();
    fpu_disable();
    sched_init();
    gic_init();
    timer_init(1000);               /* 1 kHz preemption tick */

    /* Add order fixes task ids: 0=RT, 1=NA, 2=NB, 3=IDLE. */
    process_t *rt = make_busy("rt",   RT_ITERS,   SCHED_PRIO_REALTIME);
    process_t *na = make_busy("na",   N_ITERS,    SCHED_PRIO_NORMAL);
    process_t *nb = make_busy("nb",   N_ITERS,    SCHED_PRIO_NORMAL);
    process_t *id = make_busy("idle", IDLE_ITERS, SCHED_PRIO_IDLE);

    uart_puts("running 4 EL0 busy tasks (RT, NORMAL x2, IDLE)...\r\n");
    sched_run();                    /* returns when every task has exited */
    timer_stop();

    uart_printf("ticks: RT=%d NA=%d NB=%d IDLE=%d\r\n",
                g_count[0], g_count[1], g_count[2], g_count[3]);
    uart_printf("first: RT=%d NA=%d NB=%d IDLE=%d\r\n",
                (int)g_first[0], (int)g_first[1], (int)g_first[2], (int)g_first[3]);
    uart_printf("last : RT=%d NA=%d NB=%d IDLE=%d\r\n",
                (int)g_last[0], (int)g_last[1], (int)g_last[2], (int)g_last[3]);

    int all_exited = (rt->state == PROC_ZOMBIE) && (na->state == PROC_ZOMBIE) &&
                     (nb->state == PROC_ZOMBIE) && (id->state == PROC_ZOMBIE);

    int all_ran = g_count[0] && g_count[1] && g_count[2] && g_count[3];

    /* 1. REALTIME ran to completion before either NORMAL task started. */
    int rt_first = (g_last[0] >= 0) &&
                   (g_first[1] > g_last[0]) && (g_first[2] > g_last[0]);

    /* 2. The two NORMAL tasks interleaved (overlapping lifetimes) => the timer
     *    preempted them; a cooperative run would give disjoint intervals. */
    int normal_interleaved = (g_first[1] <= g_last[2]) && (g_first[2] <= g_last[1]);

    /* 3. IDLE ran only after both NORMAL tasks were done. */
    int idle_last = (g_first[3] >= g_last[1]) && (g_first[3] >= g_last[2]);

    uart_printf("checks: exited=%d ran=%d rt-first=%d interleave=%d idle-last=%d\r\n",
                all_exited, all_ran, rt_first, normal_interleaved, idle_last);

    int ok = all_exited && all_ran && rt_first && normal_interleaved && idle_last;
    uart_puts(ok ? "PREEMPT: PASS\r\n" : "PREEMPT: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
