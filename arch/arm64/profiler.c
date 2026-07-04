/* arch/arm64/profiler.c - per-plugin profiler (Theme G, issue #129) */

#include "profiler.h"

uint32_t prof_cyc_to_us(uint64_t cycles, uint64_t cntfrq)
{
    if (cntfrq == 0u) return 0u;
    return (uint32_t)((cycles * 1000000u + cntfrq / 2u) / cntfrq);
}

int prof_build(const pt_entry_t *in, int n, uint32_t block_us, uint64_t cntfrq,
               prof_row_t *out, int cap, uint32_t *total_load)
{
    uint32_t total = 0u;
    int rows = 0;
    for (int i = 0; i < n && rows < cap; i++) {
        if (in[i].runs == 0u)
            continue;
        uint64_t mean_cyc = in[i].sum / in[i].runs;
        uint32_t mean_us = prof_cyc_to_us(mean_cyc, cntfrq);
        uint32_t load = (block_us > 0u) ? (uint32_t)((uint64_t)mean_us * 1000u / block_us) : 0u;

        out[rows].pid          = in[i].tag;
        out[rows].runs         = (uint32_t)in[i].runs;
        out[rows].mean_us      = mean_us;
        out[rows].max_us       = prof_cyc_to_us(in[i].max, cntfrq);
        out[rows].load_permille = load;
        out[rows].overruns     = (uint32_t)in[i].overruns;
        total += load;
        rows++;
    }
    if (total_load) *total_load = total;
    return rows;
}

uint32_t prof_headroom(uint32_t total_load)
{
    return total_load >= 1000u ? 0u : 1000u - total_load;
}

/* ---- rendering (libc-free) ---------------------------------------------- */

static int put_str(char *b, int cap, int off, const char *s)
{
    while (*s && off < cap - 1) b[off++] = *s++;
    return off;
}
static int put_uint(char *b, int cap, int off, uint32_t v)
{
    char tmp[12]; int t = 0;
    if (v == 0u) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10u); v /= 10u; }
    while (t > 0 && off < cap - 1) b[off++] = tmp[--t];
    return off;
}

int prof_render(const prof_row_t *r, char *buf, int cap)
{
    if (cap <= 0) return 0;
    int o = 0;
    o = put_str(buf, cap, o, "prof: pid=");   o = put_uint(buf, cap, o, r->pid);
    o = put_str(buf, cap, o, " runs=");        o = put_uint(buf, cap, o, r->runs);
    o = put_str(buf, cap, o, " mean=");        o = put_uint(buf, cap, o, r->mean_us);
    o = put_str(buf, cap, o, "us max=");       o = put_uint(buf, cap, o, r->max_us);
    o = put_str(buf, cap, o, "us load=");      o = put_uint(buf, cap, o, r->load_permille / 10u);
    o = put_str(buf, cap, o, ".");             o = put_uint(buf, cap, o, r->load_permille % 10u);
    o = put_str(buf, cap, o, "% overruns=");   o = put_uint(buf, cap, o, r->overruns);
    buf[o < cap ? o : cap - 1] = '\0';
    return o;
}
