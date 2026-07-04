/* arch/arm64/mem_quota.h - per-plugin memory quota (Theme A: reliability)
 *
 * The M12 CPU budget bounds the time a plugin may take; this bounds the memory
 * it may hold.  A plugin's footprint is fixed at load (its ELF PT_LOAD segments,
 * including .bss, plus the fixed stack / trampoline / param / parameter-queue
 * pages) - plugins cannot allocate at runtime, there is no such syscall.  So the
 * quota is enforced at load time on the image's *declared* page count, before a
 * single frame is committed: a plugin that would exceed its budget is refused
 * outright, so a greedy or hostile image cannot exhaust physical memory and
 * starve the audio engine or the other plugins.
 *
 * This completes the resource-isolation story - memory, time, and syscalls all
 * bounded per plugin.  The accounting is pure and integer-only.  It is defined
 * here as `static inline` so the plugin manager inlines it with no extra link
 * dependency, and it is unit-tested on the host (make test-arm-mem-quota).
 */

#ifndef ARM64_MEM_QUOTA_H
#define ARM64_MEM_QUOTA_H

#include <stdint.h>

typedef struct {
    uint32_t limit_pages;   /* 0 means unlimited                       */
    uint32_t used_pages;    /* pages currently charged                 */
    uint32_t denied;        /* charge requests refused (over budget)   */
} mem_quota_t;

/* Initialise with a page limit (0 = unlimited). */
static inline void mq_init(mem_quota_t *q, uint32_t limit_pages)
{
    q->limit_pages = limit_pages;
    q->used_pages  = 0;
    q->denied      = 0;
}

/* Try to charge `pages` more.  Returns 1 and charges if it fits within the
 * limit (or the limit is unlimited); returns 0 and bumps `denied`, charging
 * nothing, if it would exceed the limit. */
static inline int mq_charge(mem_quota_t *q, uint32_t pages)
{
    if (q->limit_pages != 0 &&
        (uint64_t)q->used_pages + pages > q->limit_pages) {
        q->denied++;
        return 0;
    }
    q->used_pages += pages;
    return 1;
}

/* Release `pages` previously charged (clamped at zero). */
static inline void mq_release(mem_quota_t *q, uint32_t pages)
{
    q->used_pages = (pages >= q->used_pages) ? 0 : q->used_pages - pages;
}

/* Bytes -> whole pages (ceiling), for a 4 KiB page. */
static inline uint32_t mq_bytes_to_pages(uint64_t bytes)
{
    return (uint32_t)((bytes + 4096ull - 1) / 4096ull);
}

#endif /* ARM64_MEM_QUOTA_H */
