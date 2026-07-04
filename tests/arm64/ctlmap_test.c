/* tests/arm64/ctlmap_test.c - host unit tests for the control-surface mapping
 * with MIDI-learn (Theme E, issue #120).
 *
 * Build/run via:  make test-arm-ctlmap
 */

#include "ctlmap.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static ctl_source_t SRC(ctl_src_type_t t, uint16_t id)
{
    ctl_source_t s = { t, id };
    return s;
}

static void test_continuous(void)
{
    printf("- continuous: an expression pedal scales onto the param range\n");
    ctlmap_t m; ctlmap_init(&m);
    ctl_source_t expr = SRC(CTL_SRC_EXPR, 0);
    CHECK(ctlmap_bind(&m, expr, 42, 0, 1000, CTL_MODE_CONTINUOUS) >= 0, "bind ok");

    uint32_t p; int32_t v;
    CHECK(ctlmap_feed(&m, expr, 0, &p, &v)   && p == 42 && v == 0,    "heel (0) -> out_min");
    CHECK(ctlmap_feed(&m, expr, 127, &p, &v) && v == 1000,           "toe (127) -> out_max");
    CHECK(ctlmap_feed(&m, expr, 64, &p, &v)  && v > 495 && v < 510,  "mid (~64) -> ~halfway");
    /* Clamps out-of-range input. */
    CHECK(ctlmap_feed(&m, expr, 200, &p, &v) && v == 1000, "over-range clamps to out_max");
}

static void test_momentary_and_toggle(void)
{
    printf("- footswitch: momentary follows the switch, toggle flips on press\n");
    ctlmap_t m; ctlmap_init(&m);
    ctl_source_t fs_m = SRC(CTL_SRC_FOOTSW, 1);
    ctl_source_t fs_t = SRC(CTL_SRC_FOOTSW, 2);
    ctlmap_bind(&m, fs_m, 10, 0, 1, CTL_MODE_MOMENTARY);
    ctlmap_bind(&m, fs_t, 11, 0, 1, CTL_MODE_TOGGLE);

    uint32_t p; int32_t v;
    CHECK(ctlmap_feed(&m, fs_m, 127, &p, &v) && v == 1, "momentary down -> 1");
    CHECK(ctlmap_feed(&m, fs_m, 0,   &p, &v) && v == 0, "momentary up -> 0");

    CHECK(ctlmap_feed(&m, fs_t, 127, &p, &v) && v == 1, "toggle 1st press -> 1");
    CHECK(ctlmap_feed(&m, fs_t, 0,   &p, &v) == 0,      "toggle ignores the release");
    CHECK(ctlmap_feed(&m, fs_t, 127, &p, &v) && v == 0, "toggle 2nd press -> 0");
    CHECK(ctlmap_feed(&m, fs_t, 127, &p, &v) && v == 1, "toggle 3rd press -> 1");
}

static void test_midi_learn(void)
{
    printf("- MIDI-learn: the next control fed binds to the armed parameter\n");
    ctlmap_t m; ctlmap_init(&m);
    ctl_source_t cc74 = SRC(CTL_SRC_MIDI_CC, 74);

    uint32_t p; int32_t v;
    CHECK(ctlmap_feed(&m, cc74, 100, &p, &v) == 0, "unbound control does nothing");

    ctlmap_learn_begin(&m, 7, 0, 255, CTL_MODE_CONTINUOUS);
    CHECK(ctlmap_learn_pending(&m), "learn is armed");
    /* The wiggle that assigns it also reports its value straight away. */
    CHECK(ctlmap_feed(&m, cc74, 127, &p, &v) && p == 7 && v == 255,
          "the learning control binds and reports immediately");
    CHECK(!ctlmap_learn_pending(&m), "learn disarms after one control");
    CHECK(ctlmap_find(&m, cc74) >= 0, "the control is now bound");
    CHECK(ctlmap_feed(&m, cc74, 0, &p, &v) && p == 7 && v == 0,
          "subsequent moves drive the learned parameter");

    /* Cancel disarms without binding. */
    ctlmap_learn_begin(&m, 9, 0, 1, CTL_MODE_TOGGLE);
    ctlmap_learn_cancel(&m);
    CHECK(!ctlmap_learn_pending(&m), "cancel disarms learn");
}

static void test_bind_management(void)
{
    printf("- rebinding replaces, unbind removes, the table has a capacity\n");
    ctlmap_t m; ctlmap_init(&m);
    ctl_source_t enc = SRC(CTL_SRC_ENCODER, 3);

    ctlmap_bind(&m, enc, 1, 0, 10, CTL_MODE_CONTINUOUS);
    int n1 = m.n;
    ctlmap_bind(&m, enc, 2, 0, 20, CTL_MODE_CONTINUOUS);   /* same source */
    CHECK(m.n == n1, "rebinding the same control does not grow the table");
    uint32_t p; int32_t v;
    CHECK(ctlmap_feed(&m, enc, 127, &p, &v) && p == 2 && v == 20,
          "the rebinding wins");

    CHECK(ctlmap_unbind(&m, enc) == 1, "unbind reports removal");
    CHECK(ctlmap_feed(&m, enc, 127, &p, &v) == 0, "unbound control is silent");
    CHECK(ctlmap_unbind(&m, enc) == 0, "unbinding twice is a no-op");

    /* Fill to capacity with distinct MIDI CCs. */
    ctlmap_init(&m);
    int bound = 0;
    for (int i = 0; i < CTLMAP_MAX + 4; i++)
        if (ctlmap_bind(&m, SRC(CTL_SRC_MIDI_CC, (uint16_t)i), (uint32_t)i,
                        0, 1, CTL_MODE_CONTINUOUS) >= 0)
            bound++;
    CHECK(bound == CTLMAP_MAX, "binds exactly up to the table capacity");
}

int main(void)
{
    printf("=== Tessera control-surface mapping tests (Theme E, #120) ===\n");
    test_continuous();
    test_momentary_and_toggle();
    test_midi_learn();
    test_bind_management();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
