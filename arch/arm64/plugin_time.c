/* arch/arm64/plugin_time.c - per-plugin service-time reporting (Issue #77, M12) */

#include "plugin_time.h"
#include "latency.h"

void pt_board_init(pt_board_t *b)
{
    b->seq = 0;
    b->n   = 0;
    for (int i = 0; i < AW_MAX_NODES; i++) {
        b->e[i].tag      = 0;
        b->e[i].runs     = 0;
        b->e[i].overruns = 0;
        b->e[i].offences = 0;
        b->e[i].min      = 0;
        b->e[i].max      = 0;
        b->e[i].sum      = 0;
    }
}

void pt_publish(audio_worker_t *w, void *board)
{
    pt_board_t *b = board;
    if (!b)
        return;

    uint32_t seq = __atomic_fetch_add(&b->seq, 1u, __ATOMIC_RELAXED);
    /* Publish the odd sequence before ANY payload changes.  Payload fields
     * are atomic too: rejecting a torn snapshot does not legalise C races. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t n = __atomic_load_n(&w->n_nodes, __ATOMIC_ACQUIRE);
    if (n > AW_MAX_NODES) n = AW_MAX_NODES;
    __atomic_store_n(&b->n, n, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; i++) {
        const aw_node_t *nd = &w->nodes[i];
#define PT_STORE(field, value) __atomic_store_n(&b->e[i].field, (value), __ATOMIC_RELAXED)
        PT_STORE(tag, nd->tag);
        PT_STORE(runs, nd->runs);
        PT_STORE(overruns, __atomic_load_n(&nd->overruns, __ATOMIC_RELAXED));
        PT_STORE(offences, nd->offences);
        PT_STORE(min, nd->runs ? nd->svc_min : 0);
        PT_STORE(max, nd->svc_max);
        PT_STORE(sum, nd->svc_sum);
#undef PT_STORE
    }
    __atomic_store_n(&b->seq, seq + 2u, __ATOMIC_RELEASE);
}

int pt_snapshot(const pt_board_t *b, pt_entry_t *out, int cap, int retries)
{
    if (!b || cap < 0 || (cap > 0 && !out) || retries < 1)
        return -1;
    if (cap > AW_MAX_NODES) cap = AW_MAX_NODES;
    for (int attempt = 0; attempt < retries; attempt++) {
        uint32_t s1 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u) continue;
        uint32_t count = __atomic_load_n(&b->n, __ATOMIC_RELAXED);
        int n = count > (uint32_t)cap ? cap : (int)count;
        for (int i = 0; i < n; i++) {
#define PT_LOAD(field) out[i].field = __atomic_load_n(&b->e[i].field, __ATOMIC_RELAXED)
            PT_LOAD(tag);
            PT_LOAD(runs);
            PT_LOAD(overruns);
            PT_LOAD(offences);
            PT_LOAD(min);
            PT_LOAD(max);
            PT_LOAD(sum);
#undef PT_LOAD
        }
        /* Complete payload reads before validating the sequence.  Together
         * with the writer's release fence, seeing any new payload prevents
         * accepting the preceding even sequence. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t s2 = __atomic_load_n(&b->seq, __ATOMIC_RELAXED);
        if (s1 == s2) return n;
    }
    return -1;
}

/* ---- rendering (freestanding, no printf) ------------------------------ */

static int put_str(char *out, int cap, int at, const char *str)
{
    while (*str && at < cap - 1)
        out[at++] = *str++;
    return at;
}

static int put_u64(char *out, int cap, int at, uint64_t v)
{
    char tmp[20];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    while (n && at < cap - 1)
        out[at++] = tmp[--n];
    return at;
}

int pt_render(const pt_entry_t *e, const char *name, uint64_t cntfrq,
              char *out, int cap)
{
    if (!out || cap < 1)
        return 0;

    uint64_t mean = e->runs ? e->sum / e->runs : 0;

    int at = 0;
    at = put_str(out, cap, at, "plugin_time: pid=");
    at = put_u64(out, cap, at, e->tag);
    if (name) {
        at = put_str(out, cap, at, " (");
        at = put_str(out, cap, at, name);
        at = put_str(out, cap, at, ")");
    }
    at = put_str(out, cap, at, " runs=");
    at = put_u64(out, cap, at, e->runs);
    at = put_str(out, cap, at, " min=");
    at = put_u64(out, cap, at, lat_cyc_to_us(e->min, cntfrq));
    at = put_str(out, cap, at, "us max=");
    at = put_u64(out, cap, at, lat_cyc_to_us(e->max, cntfrq));
    at = put_str(out, cap, at, "us mean=");
    at = put_u64(out, cap, at, lat_cyc_to_us(mean, cntfrq));
    at = put_str(out, cap, at, "us overruns=");
    at = put_u64(out, cap, at, e->overruns);
    at = put_str(out, cap, at, " offences=");
    at = put_u64(out, cap, at, e->offences);
    out[at] = '\0';
    return at;
}
