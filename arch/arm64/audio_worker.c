/* arch/arm64/audio_worker.c - pinned per-core audio workers (Issue #74, M11) */

#include "audio_worker.h"

/* Park/wake primitives.  On AArch64 the worker parks in WFE and a kick's SEV
 * wakes it; SEV sets the event register, so a kick landing between the
 * worker's last check and its WFE is not lost.  On the host (unit tests) the
 * "worker core" is a pthread and just spins politely. */
#if defined(__aarch64__)
static inline void aw_park(void)  { __asm__ volatile("wfe" ::: "memory"); }
static inline void aw_wake(void)  { __asm__ volatile("sev" ::: "memory"); }
#else
static inline void aw_park(void)  { __asm__ volatile("" ::: "memory"); }
static inline void aw_wake(void)  { }
#endif

void aw_init(audio_worker_t *w, uint32_t cpu_id)
{
    w->block_seq = 0;
    w->kicks     = 0;
    w->overruns  = 0;
    w->done_seq  = 0;
    w->blocks    = 0;
    w->online    = 0;
    w->stop      = 0;
    w->n_nodes   = 0;
    w->cpu_id    = cpu_id;
    w->clock     = 0;
    w->publish   = 0;
    w->pub_ctx   = 0;
    for (int i = 0; i < AW_MAX_NODES; i++) {
        w->nodes[i].run      = 0;
        w->nodes[i].ctx      = 0;
        w->nodes[i].tag      = 0;
        w->nodes[i].runs     = 0;
        w->nodes[i].overruns = 0;
        w->nodes[i].offences = 0;
        w->nodes[i].svc_min  = ~0ull;
        w->nodes[i].svc_max  = 0;
        w->nodes[i].svc_sum  = 0;
    }
}

int aw_assign(audio_worker_t *w, void (*run)(void *ctx), void *ctx)
{
    uint32_t n = __atomic_load_n(&w->n_nodes, __ATOMIC_RELAXED);
    if (!run || n >= AW_MAX_NODES)
        return -1;
    w->nodes[n].run      = run;
    w->nodes[n].ctx      = ctx;
    w->nodes[n].tag      = 0;
    w->nodes[n].runs     = 0;
    w->nodes[n].overruns = 0;
    w->nodes[n].offences = 0;
    w->nodes[n].svc_min  = ~0ull;    /* until the first timed run */
    w->nodes[n].svc_max  = 0;
    w->nodes[n].svc_sum  = 0;
    /* Publish the entry before the count so a worker that sees the new count
     * sees a complete node. */
    __atomic_store_n(&w->n_nodes, n + 1, __ATOMIC_RELEASE);
    return (int)n;
}

void aw_clear(audio_worker_t *w)
{
    __atomic_store_n(&w->n_nodes, 0, __ATOMIC_RELEASE);
}

int aw_kick(audio_worker_t *w, uint64_t seq)
{
    uint32_t n = __atomic_load_n(&w->n_nodes, __ATOMIC_ACQUIRE);
    if (n == 0)
        return 1;                       /* empty worker stays parked */
    w->kicks++;

    /* Late check: the worker must have answered everything published so far.
     * Only CPU0 writes block_seq, so it can read its own copy plainly. */
    if (__atomic_load_n(&w->done_seq, __ATOMIC_ACQUIRE) != w->block_seq) {
        w->overruns++;                  /* the block is skipped, not queued */
        for (uint32_t i = 0; i < n; i++)
            w->nodes[i].overruns++;
        return 0;
    }

    __atomic_store_n(&w->block_seq, seq, __ATOMIC_RELEASE);
    aw_wake();
    return 1;
}

int aw_worker_step(audio_worker_t *w)
{
    uint64_t seq = __atomic_load_n(&w->block_seq, __ATOMIC_ACQUIRE);
    if (seq == w->done_seq)             /* done_seq is ours alone to write */
        return 0;

    uint32_t n = __atomic_load_n(&w->n_nodes, __ATOMIC_ACQUIRE);
    if (w->clock) {
        /* Per-node service-time accounting (issue #77): two counter reads
         * and three additions per node, only when a clock is installed. */
        for (uint32_t i = 0; i < n; i++) {
            uint64_t t0 = w->clock();
            w->nodes[i].run(w->nodes[i].ctx);
            uint64_t dt = w->clock() - t0;
            if (dt < w->nodes[i].svc_min) w->nodes[i].svc_min = dt;
            if (dt > w->nodes[i].svc_max) w->nodes[i].svc_max = dt;
            w->nodes[i].svc_sum += dt;
            w->nodes[i].runs++;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            w->nodes[i].run(w->nodes[i].ctx);
            w->nodes[i].runs++;
        }
    }
    w->blocks++;
    if (w->publish)                 /* stats out before the block is answered */
        w->publish(w, w->pub_ctx);
    __atomic_store_n(&w->done_seq, seq, __ATOMIC_RELEASE);
    return 1;
}

void aw_worker_loop(audio_worker_t *w)
{
    __atomic_store_n(&w->online, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        if (!aw_worker_step(w))
            aw_park();
    }
}

void aw_stop(audio_worker_t *w)
{
    __atomic_store_n(&w->stop, 1, __ATOMIC_RELEASE);
    aw_wake();
}

int aw_drained(const audio_worker_t *w)
{
    return __atomic_load_n(&w->done_seq, __ATOMIC_ACQUIRE) == w->block_seq;
}
