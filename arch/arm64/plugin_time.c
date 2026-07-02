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

    __atomic_store_n(&b->seq, b->seq + 1u, __ATOMIC_RELEASE);      /* odd  */

    uint32_t n = w->n_nodes;
    if (n > AW_MAX_NODES)
        n = AW_MAX_NODES;
    b->n = n;
    for (uint32_t i = 0; i < n; i++) {
        const aw_node_t *nd = &w->nodes[i];
        b->e[i].tag      = nd->tag;
        b->e[i].runs     = nd->runs;
        b->e[i].overruns = nd->overruns;
        b->e[i].offences = nd->offences;
        b->e[i].min      = nd->runs ? nd->svc_min : 0;
        b->e[i].max      = nd->svc_max;
        b->e[i].sum      = nd->svc_sum;
    }

    __atomic_store_n(&b->seq, b->seq + 1u, __ATOMIC_RELEASE);      /* even */
}

int pt_snapshot(const pt_board_t *b, pt_entry_t *out, int cap, int retries)
{
    for (int attempt = 0; attempt < retries; attempt++) {
        uint32_t s1 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)
            continue;                          /* publish in progress */

        int n = (int)b->n;
        if (n > cap)
            n = cap;
        for (int i = 0; i < n; i++)
            out[i] = b->e[i];

        uint32_t s2 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
        if (s1 == s2)
            return n;                          /* stable across the copy */
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
