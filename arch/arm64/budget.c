/* arch/arm64/budget.c - per-plugin CPU budget enforcement (Issue #78, M12) */

#include "budget.h"

/* ---- policy (pure) ---------------------------------------------------- */

void budget_init(budget_t *b, uint64_t cycles, uint32_t kill_after)
{
    b->cycles     = cycles ? cycles : 1u;
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

    if (b->offences != UINT64_MAX)
        b->offences++;
    b->streak++;
    if (b->streak >= b->kill_after) {
        b->killed = 1;
        return BUDGET_KILL;
    }
    return BUDGET_MUTE;
}

int budget_execute(budget_t *b, const budget_ops_t *ops, void *ctx,
                   uint64_t *elapsed)
{
    if (elapsed) *elapsed = 0;
    if (!b || !ops || !ops->clock || !ops->run || !ops->mute || !ops->kill)
        return BUDGET_FAULT;
    if (b->killed) {
        ops->mute(ctx);
        return BUDGET_KILL;
    }
    if (!b->cycles || b->cycles > INT64_MAX) {
        b->killed = 1;
        ops->mute(ctx);
        ops->kill(ctx);
        return BUDGET_FAULT;
    }

    uint64_t t0 = ops->clock();
    budget_arm(b->cycles);
    long result = ops->run(ctx);
    budget_disarm();
    uint64_t dt = ops->clock() - t0;
    if (elapsed) *elapsed = dt;

    if (result < 0 && result != BUDGET_PREEMPTED) {
        b->killed = 1;
        ops->mute(ctx);
        ops->kill(ctx);
        return BUDGET_FAULT;
    }
    int action = budget_account(b, result == BUDGET_PREEMPTED || dt >= b->cycles);
    if (action != BUDGET_OK)
        ops->mute(ctx);             /* erase partial writes AFTER preemption */
    if (action == BUDGET_KILL)
        ops->kill(ctx);             /* real death/liveness publication */
    return action;
}

/* ---- preemption (worker core) ------------------------------------------
 *
 * Real CNTP/GIC access only in the AArch64 kernel build; the host unit
 * tests cover the policy above and drive the router logic separately. */

#if defined(__aarch64__) && !defined(HOSTTEST)

#include "exceptions.h"
#include "process.h"
#include "smp.h"
#include "el0_context.h"
#include "gic.h"
#include "budget_plugin.h"

void kernel_resume(long code);


#define SPSR_EL0_MASKED   0x3C0ull    /* EL0t, DAIF all masked (default)   */
#define SPSR_EL0_IRQ_OPEN 0x340ull    /* EL0t, IRQ unmasked for preemption */

/* Banked timer and return state are owned by the executing core. */
static uint32_t g_armed[EL0_CONTEXT_CPUS];

static inline uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

static void arm_at(uint64_t expiry)
{
    uint32_t cpu = el0_cpu_index();
    uint64_t physical, virtual;
    /* Convert the physical-counter deadline into this core's virtual domain.
     * Reading virtual first makes the tiny sampling skew conservative. */
    __asm__ volatile("isb; mrs %0, cntvct_el0; mrs %1, cntpct_el0"
                     : "=r"(virtual), "=r"(physical) :: "memory");
    uint64_t remaining = expiry > physical ? expiry - physical : 1u;
    __asm__ volatile("msr cntv_ctl_el0, %0; isb" :: "r"(0ull) : "memory");
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(virtual + remaining));
    g_user_spsr[cpu] = SPSR_EL0_IRQ_OPEN;
    g_armed[cpu] = 1;
    gic_enable_irq(BUDGET_TIMER_IRQ);
    __asm__ volatile("msr cntv_ctl_el0, %0; isb" :: "r"(1ull) : "memory");
}

void budget_arm(uint64_t cycles)
{
    arm_at(rd_cntpct() + cycles);
}

int budget_active(void)
{
    return g_armed[el0_cpu_index()] != 0;
}

void budget_disarm(void)
{
    __asm__ volatile("msr cntv_ctl_el0, %0; isb" :: "r"(0ull) : "memory");
    g_user_spsr[el0_cpu_index()] = SPSR_EL0_MASKED;
    g_armed[el0_cpu_index()] = 0;
}

int budget_timer_irq(struct trapframe *tf, uint32_t iar)
{
    if ((iar & 0x3ffu) != BUDGET_TIMER_IRQ) return 0;
    int armed = budget_active();
    budget_disarm();
    if (!armed) return 1; /* drain a stale banked PPI, never a cadence tick */

    /* Raced with the plugin's own return (we are at EL1)?  The window is
     * closed and the level source deasserted: just swallow the IRQ. */
    if ((tf->spsr_el1 & 0xFull) != 0)
        return 1;

    /* No polled UART in a budget IRQ: it makes containment latency depend
     * on console speed and can stall the audio core through shared MMIO. */
    gic_eoi(iar);                      /* we never return to arm64_irq     */
    kernel_resume(BUDGET_PREEMPTED);
    return 1;                          /* unreachable */
}

long budget_call(long (*run)(void *), void *ctx, uint64_t cycles)
{
    if (!run || !cycles || cycles > INT64_MAX) return -1;
    uint32_t cpu = el0_cpu_index();
    uint64_t saved_cval, saved_ctl, saved_spsr = g_user_spsr[cpu];
    uint32_t saved_armed = g_armed[cpu];
    __asm__ volatile("mrs %0, cntv_cval_el0; mrs %1, cntv_ctl_el0"
                     : "=r"(saved_cval), "=r"(saved_ctl));
    uint64_t start = rd_cntpct();
    if (saved_armed) {
        uint64_t now_v;
        __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now_v));
        if ((int64_t)(saved_cval - now_v) <= 0) return BUDGET_PREEMPTED;
        if (cycles > saved_cval - now_v) cycles = saved_cval - now_v;
    }
    arm_at(start + cycles);
    long result = run(ctx);
    budget_disarm();
    if (result >= 0 && rd_cntpct() - start >= cycles) result = BUDGET_PREEMPTED;
    g_user_spsr[cpu] = saved_spsr;
    g_armed[cpu] = saved_armed;
    __asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb"
                     :: "r"(saved_cval), "r"(saved_ctl & 3u) : "memory");
    return result;
}

static long plugin_run(void *ctx)
{
    budget_plugin_call_t *c = ctx;
    return plugin_call_block(c->plugin, c->in_l, c->in_r,
                             c->out_l, c->out_r, c->frames);
}

static void plugin_mute(void *ctx)
{
    budget_plugin_call_t *c = ctx;
    /* Host-owned aliases, never plugin-supplied pointers or ring cursors. */
    uint32_t *left = c->left, *right = c->right;
    for (uint32_t i = 0; i < c->frames; i++) {
        left[i] = 0;
        right[i] = 0;
    }
}

static void plugin_kill(void *ctx)
{
    budget_plugin_call_t *c = ctx;
    process_kill(c->plugin->proc, BUDGET_PREEMPTED);
}

long budget_plugin_invoke(budget_plugin_call_t *call, uint64_t cycles,
                          uint64_t cutoff, uint64_t *elapsed)
{
    if (elapsed) *elapsed = 0;
    if (!call || !call->plugin || !call->plugin->proc || !call->left ||
        !call->right || !call->frames || !cycles || cycles > INT64_MAX)
        return -1;
    uint64_t start = rd_cntpct();
    if (start >= cutoff) return BUDGET_DEADLINE;
    uint64_t available = cutoff - start;
    uint64_t limit = cycles < available ? cycles : available;
    arm_at(start + limit);
    long result = plugin_run(call);
    budget_disarm();
    uint64_t dt = rd_cntpct() - start;
    if (elapsed) *elapsed = dt;
    if (result == BUDGET_PREEMPTED || (result >= 0 && dt >= limit))
        return limit < cycles ? BUDGET_DEADLINE : BUDGET_PREEMPTED;
    return result;
}

int budget_plugin_run(budget_t *b, budget_plugin_call_t *call, uint64_t *elapsed)
{
    static const budget_ops_t ops = {
        rd_cntpct, plugin_run, plugin_mute, plugin_kill
    };
    if (!call || !call->plugin || !call->plugin->proc ||
        !call->left || !call->right || !call->frames) {
        if (elapsed) *elapsed = 0;
        return BUDGET_FAULT;
    }
    return budget_execute(b, &ops, call, elapsed);
}

#else /* host build: policy only */

void budget_arm(uint64_t cycles)   { (void)cycles; }
void budget_disarm(void)           { }

#endif
