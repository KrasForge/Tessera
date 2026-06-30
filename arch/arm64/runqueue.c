/* arch/arm64/runqueue.c - priority run queue for the RT scheduler (Issue #20)
 *
 * Pure scheduling policy: priority bands with round-robin within a band, plus
 * a priority-inheriting mutex stub.  No ARM/MMU access, so it is unit-tested
 * on the host (make test-arm-rtsched) and reused unchanged in the kernel by
 * arch/arm64/sched.c.
 *
 * Round-robin is implemented with a monotonically increasing enqueue sequence
 * number: rq_pick() chooses the runnable task in the highest non-empty band
 * with the smallest sequence (the one waiting longest), and rq_commit() stamps
 * the chosen task with a fresh (largest) sequence so it rotates to the tail.
 */

#include "runqueue.h"

typedef struct {
    uint8_t  used;
    uint8_t  runnable;
    uint8_t  base_prio;    /* assigned priority                       */
    uint8_t  prio;         /* effective priority (>= via inheritance) */
    uint32_t quantum;      /* round-robin slice length, in ticks      */
    uint32_t slice;        /* ticks remaining in the current slice    */
    uint32_t seq;          /* FIFO ordering within a band             */
    uint64_t ran_ticks;    /* total ticks scheduled (fairness stats)  */
} rq_slot_t;

static rq_slot_t g_slot[RQ_MAX_TASKS];
static uint32_t  g_seq;     /* enqueue sequence source */
static int       g_cur;     /* currently running task id, or RQ_NONE */

static int valid(int id)
{
    return id >= 0 && id < RQ_MAX_TASKS && g_slot[id].used;
}

void rq_init(void)
{
    for (int i = 0; i < RQ_MAX_TASKS; i++)
        g_slot[i] = (rq_slot_t){0};
    g_seq = 0;
    g_cur = RQ_NONE;
}

int rq_add(int id, int prio, uint32_t quantum)
{
    if (id < 0 || id >= RQ_MAX_TASKS)
        return -1;
    if (prio < 0 || prio >= SCHED_NPRIO)
        return -1;

    rq_slot_t *s = &g_slot[id];
    s->used      = 1;
    s->runnable  = 1;
    s->base_prio = (uint8_t)prio;
    s->prio      = (uint8_t)prio;
    s->quantum   = quantum ? quantum : RQ_QUANTUM_DEFAULT;
    s->slice     = s->quantum;
    s->seq       = ++g_seq;
    s->ran_ticks = 0;
    return 0;
}

void rq_block(int id)
{
    if (valid(id))
        g_slot[id].runnable = 0;
}

void rq_unblock(int id)
{
    if (valid(id) && !g_slot[id].runnable) {
        g_slot[id].runnable = 1;
        g_slot[id].seq      = ++g_seq;   /* re-enter at the tail of the band */
    }
}

void rq_remove(int id)
{
    if (valid(id)) {
        g_slot[id] = (rq_slot_t){0};
        if (g_cur == id)
            g_cur = RQ_NONE;
    }
}

void rq_set_effective_prio(int id, int prio)
{
    if (valid(id) && prio >= 0 && prio < SCHED_NPRIO)
        g_slot[id].prio = (uint8_t)prio;
}

int rq_base_prio(int id)      { return valid(id) ? g_slot[id].base_prio : RQ_NONE; }
int rq_effective_prio(int id) { return valid(id) ? g_slot[id].prio      : RQ_NONE; }
int rq_current(void)          { return g_cur; }

int rq_pick(void)
{
    int best = RQ_NONE;
    for (int i = 0; i < RQ_MAX_TASKS; i++) {
        rq_slot_t *s = &g_slot[i];
        if (!s->used || !s->runnable)
            continue;
        if (best == RQ_NONE ||
            s->prio < g_slot[best].prio ||
            (s->prio == g_slot[best].prio && s->seq < g_slot[best].seq))
            best = i;
    }
    return best;
}

void rq_commit(int id)
{
    if (!valid(id))
        return;
    g_slot[id].slice = g_slot[id].quantum;
    g_slot[id].seq   = ++g_seq;          /* rotate to the tail of the band */
    g_cur = id;
}

int rq_tick(void)
{
    int cur = g_cur;

    /* Account the tick to the running task. */
    if (valid(cur) && g_slot[cur].runnable) {
        if (g_slot[cur].slice > 0)
            g_slot[cur].slice--;
        g_slot[cur].ran_ticks++;
    }

    int best = rq_pick();
    if (best == RQ_NONE)
        return RQ_NONE;                  /* nothing runnable: go idle */

    /* Current task gone or blocked: switch to whatever is runnable. */
    if (!valid(cur) || !g_slot[cur].runnable)
        return best;

    /* A strictly higher-priority task is runnable: preempt immediately. */
    if (g_slot[best].prio < g_slot[cur].prio)
        return best;

    /* Same band: only rotate when the time slice is spent. */
    if (g_slot[cur].slice == 0) {
        if (best == cur) {               /* alone in its band: keep running */
            g_slot[cur].slice = g_slot[cur].quantum;
            return cur;
        }
        return best;                     /* round-robin to an equal peer */
    }

    return cur;                          /* keep running the current task */
}

uint64_t rq_ran_ticks(int id)
{
    return valid(id) ? g_slot[id].ran_ticks : 0;
}

/* ---- priority-inheriting mutex stub ---------------------------------------- */

void rq_mutex_init(rq_mutex_t *m)
{
    m->holder = RQ_NONE;
    m->waiter = RQ_NONE;
}

int rq_mutex_lock(rq_mutex_t *m, int id)
{
    if (m->holder == RQ_NONE) {
        m->holder = id;
        return 1;                        /* acquired uncontended */
    }

    /* Contended: block the requester.  If it outranks the holder, the holder
     * inherits the requester's priority so it cannot be preempted by an
     * unrelated mid-priority task while holding the lock (priority inversion
     * avoidance). */
    if (valid(id) && valid(m->holder) &&
        g_slot[id].prio < g_slot[m->holder].prio)
        rq_set_effective_prio(m->holder, g_slot[id].prio);

    /* Track the highest-priority waiter (stub: a single inheriting waiter). */
    if (m->waiter == RQ_NONE ||
        (valid(id) && valid(m->waiter) && g_slot[id].prio < g_slot[m->waiter].prio))
        m->waiter = id;

    rq_block(id);
    return 0;                            /* must wait */
}

void rq_mutex_unlock(rq_mutex_t *m, int id)
{
    if (m->holder != id)
        return;

    /* Drop any inherited priority back to the holder's base. */
    if (valid(id))
        rq_set_effective_prio(id, g_slot[id].base_prio);

    if (m->waiter != RQ_NONE) {
        int w = m->waiter;
        m->holder = w;                   /* hand the lock to the waiter */
        m->waiter = RQ_NONE;
        rq_unblock(w);
    } else {
        m->holder = RQ_NONE;
    }
}
