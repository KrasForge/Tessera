/* arch/arm64/budget.c - per-plugin CPU budget enforcement (Issue #78, M12) */

#include "budget.h"

/* ---- policy (pure) ---------------------------------------------------- */

void budget_init(budget_t *b, uint64_t cycles, uint32_t kill_after)
{
    b->cycles     = cycles;
    b->kill_after = kill_after ? kill_after : 1u;
    b->streak     = 0;
    b->offences   = 0;
    b->killed     = 0;
}

uint64_t budget_fair_share(uint64_t block_cycles, uint32_t n_nodes)
{
    if (n_nodes == 0)
        n_nodes = 1;
    uint64_t share = block_cycles / n_nodes;
    return share ? share : 1;
}

int budget_account(budget_t *b, int overran)
{
    if (b->killed)
        return BUDGET_KILL;            /* latched: dead plugins stay dead */

    if (!overran) {
        b->streak = 0;                 /* a clean block is forgiveness    */
        return BUDGET_OK;
    }

    b->offences++;
    b->streak++;
    if (b->streak >= b->kill_after) {
        b->killed = 1;
        return BUDGET_KILL;
    }
    return BUDGET_MUTE;
}

/* ---- preemption (worker core) ------------------------------------------
 *
 * Real CNTP/GIC access only in the AArch64 kernel build; the host unit
 * tests cover the policy above and drive the router logic separately. */

#if defined(__aarch64__) && !defined(HOSTTEST)

#include "exceptions.h"
#include "process.h"
#include "smp.h"
#include "gic.h"
#include "uart_pl011.h"

void kernel_resume(long code);
extern uint64_t g_user_spsr;           /* entry.S: SPSR for the EL0 entry  */

#define SPSR_EL0_MASKED   0x3C0ull    /* EL0t, DAIF all masked (default)   */
#define SPSR_EL0_IRQ_OPEN 0x340ull    /* EL0t, IRQ unmasked for preemption */

/* Single armed slot, tagged with the owning core (cpu_id + 1; 0 = none) -
 * one core runs EL0 plugins at a time (see header). */
static volatile uint32_t g_armed_cpu;

static inline uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void budget_arm(uint64_t cycles)
{
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(rd_cntpct() + cycles));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(1ull));   /* enable    */
    g_user_spsr = SPSR_EL0_IRQ_OPEN;
    __atomic_store_n(&g_armed_cpu, smp_cpu_id() + 1u, __ATOMIC_RELEASE);
}

void budget_disarm(void)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(0ull));   /* deassert  */
    g_user_spsr = SPSR_EL0_MASKED;
    __atomic_store_n(&g_armed_cpu, 0u, __ATOMIC_RELEASE);
}

int budget_timer_irq(struct trapframe *tf, uint32_t iar)
{
    if (__atomic_load_n(&g_armed_cpu, __ATOMIC_ACQUIRE) != smp_cpu_id() + 1u)
        return 0;                      /* the cadence timer's tick (CPU0)  */

    budget_disarm();

    /* Raced with the plugin's own return (we are at EL1)?  The window is
     * closed and the level source deasserted: just swallow the IRQ. */
    if ((tf->spsr_el1 & 0xFull) != 0)
        return 1;

    process_t *p = current_process();
    uart_puts("  [budget] preempting EL0 process");
    if (p)
        uart_printf(" pid=%u (%s)", (unsigned)p->pid, p->name);
    uart_puts(" at its budget boundary\r\n");

    gic_eoi(iar);                      /* we never return to arm64_irq     */
    kernel_resume(BUDGET_PREEMPTED);
    return 1;                          /* unreachable */
}

#else /* host build: policy only */

void budget_arm(uint64_t cycles)   { (void)cycles; }
void budget_disarm(void)           { }

#endif
