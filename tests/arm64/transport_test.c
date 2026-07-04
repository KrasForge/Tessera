/* tests/arm64/transport_test.c - host unit tests for the master transport
 * (Theme C, issue #114).
 *
 * The transport is a fixed-point musical clock; it is checked here for exact
 * behaviour: position advances at the set tempo, bar/beat accounting is correct
 * across time signatures, an incoming MIDI clock sets the tempo and steps the
 * position, MIDI clock out fires at 24 PPQN, and start/continue/stop behave.
 *
 * Build/run via:  make test-arm-transport
 */

#include "transport.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR 48000u

static uint32_t total_ticks(const transport_t *t)
{
    uint32_t tpb = transport_ticks_per_beat(t);
    return ((t->bar * t->num) + t->beat) * tpb + t->tick;
}

static void test_init(void)
{
    printf("- init: bar 0, stopped, 4/4\n");
    transport_t t; transport_init(&t, SR, 120000u);
    CHECK(t.bar == 0 && t.beat == 0 && t.tick == 0, "position at origin");
    CHECK(!t.playing, "stopped");
    CHECK(t.num == 4 && t.den == 4, "4/4 default");
    CHECK(transport_ticks_per_beat(&t) == TP_PPQ, "a beat is one quarter (TP_PPQ ticks)");
    transport_advance(&t, SR);
    CHECK(total_ticks(&t) == 0, "advance while stopped does nothing");
}

static void test_advance_tempo(void)
{
    printf("- advance at 120 BPM: 1 second is exactly 2 beats\n");
    transport_t t; transport_init(&t, SR, 120000u);
    transport_start(&t);
    transport_advance(&t, SR);                 /* one second */
    CHECK(total_ticks(&t) == 2u * TP_PPQ, "1 s @120 BPM == 2 quarters (192 ticks)");
    CHECK(t.bar == 0 && t.beat == 2 && t.tick == 0, "bar 0 beat 2 tick 0");

    /* advancing in many small blocks lands at the same place (no drift) */
    transport_t t2; transport_init(&t2, SR, 120000u); transport_start(&t2);
    for (int i = 0; i < 1000; i++) transport_advance(&t2, 48);   /* 1000*48 = 48000 */
    CHECK(total_ticks(&t2) == 2u * TP_PPQ, "block-by-block matches one big block");
}

static void test_bar_rollover(void)
{
    printf("- bar rollover in 4/4\n");
    transport_t t; transport_init(&t, SR, 120000u); transport_start(&t);
    transport_advance(&t, 2u * SR);            /* 2 s @120 = 4 beats = 1 bar */
    CHECK(t.bar == 1 && t.beat == 0 && t.tick == 0, "after 4 beats -> bar 1 beat 0");
}

static void test_timesig(void)
{
    printf("- time signature 6/8\n");
    transport_t t; transport_init(&t, SR, 120000u);
    transport_set_timesig(&t, 6, 8);
    CHECK(transport_ticks_per_beat(&t) == TP_PPQ / 2u, "a beat is an eighth (TP_PPQ/2)");
    transport_start(&t);
    /* 6 eighths = 3 quarters = 1.5 s @120 -> one bar */
    transport_advance(&t, SR + SR / 2u);
    CHECK(t.bar == 1 && t.beat == 0, "6 eighths fill one 6/8 bar");
}

static void test_midi_clock_in(void)
{
    printf("- slave to incoming MIDI clock: tempo + position\n");
    transport_t t; transport_init(&t, SR, 100000u);   /* start at 100 BPM */
    /* 120 BPM -> quarter = 0.5 s = 24000 frames -> 1000 frames per clock */
    for (int i = 0; i < 24; i++) transport_midi_clock_in(&t, 1000u);
    CHECK(t.tempo_mbpm == 120000u, "tempo estimated as 120.000 BPM from the clock");
    CHECK(t.clocked == 1, "marked as externally clocked");
    CHECK(total_ticks(&t) == TP_PPQ, "24 clocks advanced exactly one quarter");
}

static void test_midi_clock_out(void)
{
    printf("- emit MIDI clock at 24 PPQN\n");
    transport_t t; transport_init(&t, SR, 120000u); transport_start(&t);
    transport_advance(&t, SR / 2u);            /* one quarter (0.5 s @120) */
    int pulses = 0;
    while (transport_clock_out(&t)) pulses++;
    CHECK(pulses == 24, "one quarter emitted 24 clock pulses");
    CHECK(transport_clock_out(&t) == 0, "no pulses left after draining");
}

static void test_control(void)
{
    printf("- start / continue / stop\n");
    transport_t t; transport_init(&t, SR, 120000u);
    transport_start(&t);
    transport_advance(&t, SR);
    CHECK(t.playing && t.beat == 2, "playing and advanced");
    transport_stop(&t);
    uint32_t at = total_ticks(&t);
    transport_advance(&t, SR);
    CHECK(!t.playing && total_ticks(&t) == at, "stop freezes the position");
    transport_continue(&t);
    transport_advance(&t, SR);
    CHECK(t.playing && total_ticks(&t) > at, "continue resumes from here");
    transport_start(&t);
    CHECK(total_ticks(&t) == 0, "start rewinds to the origin");
}

static void test_snapshot(void)
{
    printf("- snapshot mirrors the plugin-facing transport\n");
    transport_t t; transport_init(&t, SR, 128000u); transport_start(&t);
    transport_advance(&t, SR / 4u);
    transport_snapshot_t s; transport_snapshot(&t, &s);
    CHECK(s.flags == TP_PLAYING, "flags: playing");
    CHECK(s.tempo_mbpm == 128000u, "tempo 128 BPM");
    CHECK(s.ppq == TP_PPQ, "ppq is TP_PPQ");
    CHECK(s.bar == t.bar && s.beat == t.beat && s.tick == t.tick, "position matches");
}

int main(void)
{
    printf("=== Tessera master-transport tests (Theme C, #114) ===\n");
    test_init();
    test_advance_tempo();
    test_bar_rollover();
    test_timesig();
    test_midi_clock_in();
    test_midi_clock_out();
    test_control();
    test_snapshot();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
