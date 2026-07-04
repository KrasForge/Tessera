/* arch/arm64/io_quota.c - per-plugin syscall / I/O-rate quota (Theme M22,
 * issue #198).  See io_quota.h. */

#include "io_quota.h"

void ioq_init(io_quota_t *q, uint32_t ceiling, uint32_t kill_after)
{
    q->ceiling    = ceiling;
    q->kill_after = kill_after ? kill_after : 1u;
    q->used       = 0;
    q->peak       = 0;
    q->streak     = 0;
    q->hit        = 0;
    q->throttled  = 0;
    q->offences   = 0;
    q->killed     = 0;
}

int ioq_charge(io_quota_t *q, uint32_t units)
{
    if (q->killed) {                     /* a killed plugin is charged nothing */
        q->hit = 1;
        return 0;
    }
    if (q->ceiling != 0 &&
        (uint64_t)q->used + units > q->ceiling) {
        q->throttled += units;           /* refuse the whole request           */
        q->hit = 1;
        return 0;
    }
    q->used += units;
    if (q->used > q->peak)
        q->peak = q->used;
    return 1;
}

int ioq_window(io_quota_t *q)
{
    int verdict;

    if (q->killed) {
        verdict = IOQ_KILL;              /* latched: dead plugins stay dead    */
    } else if (!q->hit) {
        q->streak = 0;                   /* a clean window is forgiveness       */
        verdict = IOQ_OK;
    } else {
        q->offences++;
        q->streak++;
        if (q->streak >= q->kill_after) {
            q->killed = 1;
            verdict = IOQ_KILL;
        } else {
            verdict = IOQ_THROTTLE;
        }
    }

    q->used = 0;                         /* fresh allowance for the next window */
    q->hit  = 0;
    return verdict;
}

/* ---- rendering (libc-free, mirrors profiler.c's put_str/put_uint) -------- */

static int put_str(char *b, int cap, int off, const char *s)
{
    while (*s && off < cap - 1) b[off++] = *s++;
    return off;
}
static int put_uint(char *b, int cap, int off, uint64_t v)
{
    char tmp[20]; int t = 0;
    if (v == 0u) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10u); v /= 10u; }
    while (t > 0 && off < cap - 1) b[off++] = tmp[--t];
    return off;
}

int ioq_render(const io_quota_t *q, uint32_t pid, char *buf, int cap)
{
    if (cap <= 0) return 0;
    int o = 0;
    o = put_str(buf, cap, o, "ioq: pid=");    o = put_uint(buf, cap, o, pid);
    o = put_str(buf, cap, o, " used=");        o = put_uint(buf, cap, o, q->used);
    o = put_str(buf, cap, o, "/");             o = put_uint(buf, cap, o, q->ceiling);
    o = put_str(buf, cap, o, " peak=");        o = put_uint(buf, cap, o, q->peak);
    o = put_str(buf, cap, o, " throttled=");   o = put_uint(buf, cap, o, q->throttled);
    o = put_str(buf, cap, o, " offences=");    o = put_uint(buf, cap, o, q->offences);
    o = put_str(buf, cap, o, " killed=");      o = put_uint(buf, cap, o, q->killed);
    buf[o < cap ? o : cap - 1] = '\0';
    return o;
}
