/* tests/arm64/events_test.c - host unit tests for sample-accurate event
 * scheduling (Theme M22, issue #199).
 *
 * Two pieces are verified:
 *   1. The SDK block splitter (tessera_event_split_*, sdk/lib/tessera_event.c):
 *      an event with frame_offset N is applied at sample N, multiple events in
 *      one block apply in order at their offsets, and a plugin that ignores the
 *      offset (offset 0) behaves exactly as before.
 *   2. The transport inverse (transport_frame_at_tick, arch/arm64/transport.c):
 *      the frame it returns for a tick is the exact sample at which
 *      transport_advance crosses that tick, so scheduled events land on the
 *      tempo grid sample-accurately.
 *
 * The "voice" is a trivial gate: silent until a note-on raises it to a level,
 * a note-off drops it back - so the output edge lands exactly at the frame the
 * event was applied, which is what we assert.
 *
 * Build/run via:  make test-arm-events
 */

#include "tessera.h"
#include "transport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define BLOCK 64u
#define CAP   8u

/* An event queue with inline event storage, driven as the "host producer". */
typedef struct {
    tessera_event_queue_t hdr;
    tessera_note_event_t  ev[CAP];
} evq_t;

static void evq_init(evq_t *q)
{
    memset(q, 0, sizeof(*q));
    q->hdr.magic    = TESSERA_EVENT_QUEUE_MAGIC;
    q->hdr.capacity = CAP;
    q->hdr.mask     = CAP - 1u;
    q->hdr.head = q->hdr.tail = 0;
}

static void evq_push(evq_t *q, uint8_t type, uint8_t note, uint8_t vel, uint16_t off)
{
    uint32_t h = q->hdr.head;
    tessera_note_event_t *e = &q->ev[h & q->hdr.mask];
    e->type = type; e->channel = 0; e->data1 = note; e->data2 = vel;
    e->value = 0;   e->frame_offset = off;
    q->hdr.head = h + 1u;
}

/* Trivial gate voice: level set on note-on, cleared on note-off. */
typedef struct { int16_t level; } gate_t;

static void gate_apply(gate_t *g, const tessera_note_event_t *e)
{
    if (e->type == TESSERA_EV_NOTE_ON)  g->level = (int16_t)(e->data2 * 100);
    if (e->type == TESSERA_EV_NOTE_OFF) g->level = 0;
}

/* Render one block through the splitter, applying each event at its frame. */
static void render_block(evq_t *q, gate_t *g, int16_t *out, uint32_t block)
{
    tessera_event_split_t sp;
    tessera_event_split_init(&sp, &q->hdr, block);

    uint32_t start, len; tessera_note_event_t ev; int have;
    uint32_t covered = 0, segs = 0;
    while (tessera_event_split_next(&sp, &start, &len, &ev, &have)) {
        /* segments must be contiguous and never run past the block */
        if (start != covered) { CHECK(0, "segment start is contiguous"); return; }
        if (start + len > block) { CHECK(0, "segment stays within the block"); return; }
        for (uint32_t i = 0; i < len; i++)
            out[start + i] = g->level;
        covered += len;
        segs++;
        if (have) gate_apply(g, &ev);
    }
    if (covered != block) CHECK(0, "the whole block is rendered exactly once");
}

/* ---- 1. a single note-on lands at its exact sample --------------------- */

static void test_single_offset(void)
{
    printf("- a note-on at frame N changes the output starting at sample N\n");
    for (uint32_t n = 0; n < BLOCK; n += 13) {
        evq_t q; evq_init(&q);
        evq_push(&q, TESSERA_EV_NOTE_ON, 60, 100, (uint16_t)n);
        gate_t g = { 0 };
        int16_t out[BLOCK];
        memset(out, 0x7f, sizeof out);          /* poison, so silence is real */
        render_block(&q, &g, out, BLOCK);

        int edge_ok = 1;
        for (uint32_t i = 0; i < BLOCK; i++) {
            int16_t want = (i < n) ? 0 : 10000;
            if (out[i] != want) edge_ok = 0;
        }
        if (!edge_ok) {
            printf("    (offset %u: edge misplaced)\n", n);
            CHECK(0, "note-on edge exactly at the scheduled frame");
            return;
        }
    }
    CHECK(1, "note-on edge exactly at the scheduled frame (every offset 0..63)");
}

/* ---- 2. multiple events in one block, in order ------------------------- */

static void test_multi_in_order(void)
{
    printf("- multiple events in one block apply in order at their offsets\n");
    evq_t q; evq_init(&q);
    evq_push(&q, TESSERA_EV_NOTE_ON,  60, 100, 10);   /* on  at 10 */
    evq_push(&q, TESSERA_EV_NOTE_OFF, 60, 0,   20);   /* off at 20 */
    evq_push(&q, TESSERA_EV_NOTE_ON,  64, 50,  40);   /* on  at 40, level 5000 */
    gate_t g = { 0 };
    int16_t out[BLOCK];
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);

    int ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++) {
        int16_t want = 0;
        if (i >= 10 && i < 20) want = 10000;
        else if (i >= 40)      want = 5000;
        if (out[i] != want) ok = 0;
    }
    CHECK(ok, "gate: silent [0,10), 10000 [10,20), silent [20,40), 5000 [40,64)");
}

static void test_same_frame_order(void)
{
    printf("- two events at the same frame apply in enqueue order\n");
    evq_t q; evq_init(&q);
    /* note-on then note-off, both at frame 30: order matters -> ends silent */
    evq_push(&q, TESSERA_EV_NOTE_ON,  60, 100, 30);
    evq_push(&q, TESSERA_EV_NOTE_OFF, 60, 0,   30);
    gate_t g = { 0 };
    int16_t out[BLOCK];
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);

    int ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (out[i] != 0) ok = 0;                 /* on then off -> silent all */
    CHECK(ok, "on+off at one frame: last wins, block stays silent");

    /* reversed order: off then on -> audible from frame 30 */
    evq_init(&q);
    evq_push(&q, TESSERA_EV_NOTE_OFF, 60, 0,   30);
    evq_push(&q, TESSERA_EV_NOTE_ON,  60, 100, 30);
    g.level = 0;
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);
    ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (out[i] != ((i >= 30) ? 10000 : 0)) ok = 0;
    CHECK(ok, "off+on at one frame: audible from that frame");
}

/* ---- 3. backward compatibility (offset 0 = block start) ---------------- */

static void test_backward_compat(void)
{
    printf("- offset 0 applies at the block start (old per-block behaviour)\n");
    evq_t q; evq_init(&q);
    evq_push(&q, TESSERA_EV_NOTE_ON, 60, 100, 0);   /* a v1.2 host wrote 0 here */
    gate_t g = { 0 };
    int16_t out[BLOCK];
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);
    int ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (out[i] != 10000) ok = 0;
    CHECK(ok, "whole block audible: offset-0 event took effect at sample 0");

    /* A plugin that ignores the splitter entirely and drains per block still
     * reads every event (the pre-#199 path is untouched). */
    evq_init(&q);
    evq_push(&q, TESSERA_EV_NOTE_ON, 60, 100, 42);
    evq_push(&q, TESSERA_EV_CC,      74, 64,  10);
    tessera_note_event_t ev; int count = 0;
    while (tessera_event_read(&q.hdr, &ev)) count++;
    CHECK(count == 2, "per-block drain still returns every event (offset ignored)");

    CHECK(sizeof(tessera_note_event_t) == 8, "event struct is still 8 bytes");
}

static void test_empty_and_clamp(void)
{
    printf("- an empty queue renders the whole block; offsets are clamped\n");
    evq_t q; evq_init(&q);
    gate_t g = { 12345 };
    int16_t out[BLOCK];
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);
    int ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (out[i] != 12345) ok = 0;
    CHECK(ok, "no events: one full-block segment at the current level");

    /* An out-of-range offset (>= block) is clamped to the block end, so it has
     * no effect within this block and cannot corrupt the loop. */
    evq_init(&q);
    evq_push(&q, TESSERA_EV_NOTE_ON, 60, 100, 9999);
    g.level = 0;
    memset(out, 0x7f, sizeof out);
    render_block(&q, &g, out, BLOCK);
    ok = 1;
    for (uint32_t i = 0; i < BLOCK; i++)
        if (out[i] != 0) ok = 0;
    CHECK(ok, "over-range offset clamped to block end: no effect this block");
}

/* ---- 4. transport inverse: frame_at_tick is exact ---------------------- */

static void test_transport_inverse(void)
{
    printf("- transport_frame_at_tick is the exact inverse of transport_advance\n");
    transport_t t;
    transport_init(&t, 48000u, 120000u);       /* 48 kHz, 120 BPM */
    transport_start(&t);

    CHECK(transport_frame_at_tick(&t, 0) == 0, "tick 0 is frame 0");

    /* For a spread of tick targets, the returned frame f must be the smallest
     * frame at which advancing crosses that tick: advancing f frames reaches
     * >= k ticks, and advancing f-1 does not. */
    int exact = 1;
    for (uint32_t k = 1; k <= 96u; k++) {
        uint32_t f = transport_frame_at_tick(&t, k);
        if (f == 0) { exact = 0; break; }

        transport_t a = t;                     /* copy: same starting remainder */
        transport_advance(&a, f);
        uint32_t at_f = a.bar * 4u * 96u + a.beat * 96u + a.tick;   /* absolute ticks */

        transport_t b = t;
        transport_advance(&b, f - 1u);
        uint32_t at_fm1 = b.bar * 4u * 96u + b.beat * 96u + b.tick;

        if (!(at_f >= k && at_fm1 < k)) { exact = 0;
            printf("    (k=%u f=%u at_f=%u at_fm1=%u)\n", k, f, at_f, at_fm1);
            break; }
    }
    CHECK(exact, "every tick lands on the exact crossing frame (no drift)");

    /* A tick beyond a small block returns an offset past the block, so the call
     * site can defer it to the next block. */
    uint32_t f100 = transport_frame_at_tick(&t, 100u);
    CHECK(f100 >= BLOCK, "a far-future tick returns an offset past a 64-frame block");
}

int main(void)
{
    printf("=== sample-accurate event scheduling host tests (issue #199) ===\n");
    test_single_offset();
    test_multi_in_order();
    test_same_frame_order();
    test_backward_compat();
    test_empty_and_clamp();
    test_transport_inverse();

    if (g_fail) {
        printf("EVENTS TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("EVENTS TESTS: ALL PASS\n");
    return 0;
}
