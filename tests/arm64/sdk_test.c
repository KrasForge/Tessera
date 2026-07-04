/* tests/arm64/sdk_test.c - host unit tests for the Tessera SDK library
 * (Issue #38).
 *
 * Checks the pure helpers in libtessera.a: tessera_sinf() against the C library
 * sinf() within tolerance, tessera_clampf() at and past its bounds, and
 * tessera_param_queue_read() draining a hand-built queue in the exact wire
 * format the host produces.
 *
 * Build/run via:  make test-sdk
 */

#include "tessera.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

int main(void)
{
    printf("=== Tessera SDK library tests (issue #38) ===\n");

    /* ---- tessera_sinf: accurate and range-reduced ---- */
    float max_err = 0.0f;
    for (int i = -2000; i <= 2000; i++) {
        float x = (float)i * 0.01f;              /* [-20, 20] radians */
        float e = tessera_sinf(x) - sinf(x);
        if (e < 0) e = -e;
        if (e > max_err) max_err = e;
    }
    printf("  tessera_sinf peak abs error over [-20,20] = %.5f\n", max_err);
    CHECK(max_err < 0.002f, "tessera_sinf within 0.002 of sinf (range-reduced)");
    CHECK(tessera_sinf(0.0f) == 0.0f, "sin(0) == 0");

    /* ---- tessera_clampf ---- */
    CHECK(tessera_clampf(0.5f, 0.0f, 1.0f) == 0.5f, "clamp: in range unchanged");
    CHECK(tessera_clampf(-1.0f, 0.0f, 1.0f) == 0.0f, "clamp: below -> lo");
    CHECK(tessera_clampf(2.0f, 0.0f, 1.0f) == 1.0f, "clamp: above -> hi");

    /* ---- tessera_param_queue_read: drain a host-format queue ---- */
    struct {
        tessera_param_queue_t hdr;
        tessera_param_event_t ev[4];
    } q;
    memset(&q, 0, sizeof(q));
    q.hdr.magic = TESSERA_PARAM_QUEUE_MAGIC;
    q.hdr.capacity = 4;
    q.hdr.mask = 3;
    /* Producer pushes two events: (id 0, 880.0f) and (id 7, 0.5f). */
    union { uint32_t u; float f; } a = { .f = 880.0f }, b = { .f = 0.5f };
    q.ev[0].id = 0; q.ev[0].bits = a.u;
    q.ev[1].id = 7; q.ev[1].bits = b.u;
    q.hdr.head = 2;               /* two events published */
    q.hdr.tail = 0;

    uint32_t id; float v;
    CHECK(tessera_param_queue_read(&q.hdr, &id, &v) == 1 && id == 0 && v == 880.0f,
          "queue read: first event (id 0, 880.0)");
    CHECK(tessera_param_queue_read(&q.hdr, &id, &v) == 1 && id == 7 && v == 0.5f,
          "queue read: second event (id 7, 0.5)");
    CHECK(tessera_param_queue_read(&q.hdr, &id, &v) == 0, "queue read: empty -> 0");

    /* A wrong/zero magic is treated as empty (never reads garbage). */
    q.hdr.magic = 0; q.hdr.head = 4;
    CHECK(tessera_param_queue_read(&q.hdr, &id, &v) == 0, "queue read: bad magic -> 0");

    /* ---- ABI compat: v1.1 events (issue #124), v1.2 per-note expression (#171) ---- */
    CHECK(tessera_abi_compatible((1u << 16) | 0u) == 1, "v1.0 plugin accepted on this host");
    CHECK(tessera_abi_compatible((1u << 16) | 1u) == 1, "v1.1 plugin accepted");
    CHECK(tessera_abi_compatible((1u << 16) | 2u) == 1, "v1.2 plugin accepted (current minor)");
    CHECK(tessera_abi_compatible((1u << 16) | 3u) == 0, "v1.3 (newer minor) refused");
    CHECK(tessera_abi_compatible((2u << 16) | 0u) == 0, "v2.0 (newer major) refused");

    struct {
        tessera_event_queue_t hdr;
        tessera_note_event_t  ev[4];
    } eq;
    memset(&eq, 0, sizeof(eq));
    eq.hdr.magic = TESSERA_EVENT_QUEUE_MAGIC;
    eq.hdr.capacity = 4; eq.hdr.mask = 3;
    eq.hdr.transport.flags = TESSERA_TRANSPORT_PLAYING;
    eq.hdr.transport.tempo_mbpm = 120000;   /* 120 BPM */
    eq.hdr.transport.ppq = 96;
    eq.ev[0].type = TESSERA_EV_NOTE_ON; eq.ev[0].data1 = 60; eq.ev[0].data2 = 100;
    eq.ev[1].type = TESSERA_EV_CC;      eq.ev[1].data1 = 74; eq.ev[1].data2 = 64;
    eq.hdr.head = 2; eq.hdr.tail = 0;

    tessera_note_event_t ev;
    CHECK(tessera_event_read(&eq.hdr, &ev) == 1 && ev.type == TESSERA_EV_NOTE_ON &&
          ev.data1 == 60 && ev.data2 == 100, "event read: note-on 60 vel 100");
    CHECK(tessera_event_read(&eq.hdr, &ev) == 1 && ev.type == TESSERA_EV_CC &&
          ev.data1 == 74 && ev.data2 == 64, "event read: CC 74 = 64");
    CHECK(tessera_event_read(&eq.hdr, &ev) == 0, "event read: empty -> 0");

    tessera_transport_t tr;
    tessera_transport_read(&eq.hdr, &tr);
    CHECK((tr.flags & TESSERA_TRANSPORT_PLAYING) && tr.tempo_mbpm == 120000 && tr.ppq == 96,
          "transport read: playing, 120 BPM, 96 ppq");

    eq.hdr.magic = 0;
    CHECK(tessera_event_read(&eq.hdr, &ev) == 0, "event read: bad magic -> 0");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
