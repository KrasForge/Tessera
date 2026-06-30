/* tests/arm64/midi_test.c - host unit tests for the MIDI parser and event
 * ring (Issue #31).
 *
 * Covers Note On/Off (including the velocity-0 = Note Off convention), Control
 * Change, MIDI running status, System Real-Time bytes interleaved inside a
 * message, orphan data bytes, the lock-free event ring, and a no-drop check at
 * a rapid key-press rate.
 *
 * Build/run via:  make test-arm-midi
 */

#include "midi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Feed a byte buffer through the parser, collecting emitted events. */
static int run_stream(const uint8_t *bytes, int n, midi_event_t *out, int max)
{
    midi_parser_t p;
    midi_parser_init(&p);
    int e = 0;
    for (int i = 0; i < n; i++) {
        midi_event_t ev;
        if (midi_parse_byte(&p, bytes[i], &ev) && e < max)
            out[e++] = ev;
    }
    return e;
}

static void test_basic(void)
{
    printf("- Note On / Note Off / CC\n");
    midi_event_t ev[8];
    /* Note On ch3 note60 vel100; Note Off ch3 note60 vel0; CC ch1 cc7 val127 */
    uint8_t s[] = { 0x92, 60, 100,  0x82, 60, 0,  0xB1, 7, 127 };
    int n = run_stream(s, sizeof(s), ev, 8);
    CHECK(n == 3, "three events parsed");
    CHECK(ev[0].type == MIDI_NOTE_ON && ev[0].channel == 2 &&
          ev[0].data1 == 60 && ev[0].data2 == 100, "Note On decoded");
    CHECK(ev[1].type == MIDI_NOTE_OFF && ev[1].data1 == 60, "Note Off decoded");
    CHECK(ev[2].type == MIDI_CC && ev[2].channel == 1 &&
          ev[2].data1 == 7 && ev[2].data2 == 127, "CC decoded");
}

static void test_velocity0_off(void)
{
    printf("- Note On with velocity 0 is a Note Off\n");
    midi_event_t ev[4];
    uint8_t s[] = { 0x90, 64, 0 };
    int n = run_stream(s, sizeof(s), ev, 4);
    CHECK(n == 1 && ev[0].type == MIDI_NOTE_OFF && ev[0].data1 == 64,
          "vel-0 Note On -> Note Off");
}

static void test_running_status(void)
{
    printf("- running status (status byte sent once)\n");
    midi_event_t ev[8];
    /* One 0x90 status, then three note pairs as bare data bytes. */
    uint8_t s[] = { 0x90, 60, 100,  60, 0,  62, 100,  62, 0 };
    int n = run_stream(s, sizeof(s), ev, 8);
    CHECK(n == 4, "four events under running status");
    CHECK(ev[0].type == MIDI_NOTE_ON  && ev[0].data1 == 60, "on 60");
    CHECK(ev[1].type == MIDI_NOTE_OFF && ev[1].data1 == 60, "off 60 (running)");
    CHECK(ev[2].type == MIDI_NOTE_ON  && ev[2].data1 == 62, "on 62 (running)");
    CHECK(ev[3].type == MIDI_NOTE_OFF && ev[3].data1 == 62, "off 62 (running)");
}

static void test_realtime_interleave(void)
{
    printf("- System Real-Time byte interleaved in a message\n");
    midi_event_t ev[4];
    /* Clock byte 0xF8 lands between the note and velocity bytes. */
    uint8_t s[] = { 0x90, 60, 0xF8, 100 };
    int n = run_stream(s, sizeof(s), ev, 4);
    CHECK(n == 1 && ev[0].type == MIDI_NOTE_ON && ev[0].data2 == 100,
          "real-time byte ignored, note completes");
}

static void test_garbage(void)
{
    printf("- orphan data and system-common handling\n");
    midi_event_t ev[4];
    uint8_t s1[] = { 60, 100, 7 };               /* data with no status */
    CHECK(run_stream(s1, sizeof(s1), ev, 4) == 0, "orphan data bytes ignored");

    /* System Common (0xF1 quarter-frame) cancels running status. */
    uint8_t s2[] = { 0x90, 60, 100,  0xF1, 0,  62, 100 };
    int n = run_stream(s2, sizeof(s2), ev, 4);
    CHECK(n == 1 && ev[0].data1 == 60, "running status cleared by system-common");
}

static void test_ring(void)
{
    printf("- lock-free event ring\n");
    enum { CAP = 4 };
    unsigned char store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];
    midi_ring_t *r = (midi_ring_t *)store;
    midi_ring_init(r, CAP);

    midi_event_t e = { MIDI_NOTE_ON, 0, 60, 100 }, o;
    CHECK(midi_ring_pop(r, &o) == 0, "pop empty fails");
    for (int i = 0; i < CAP; i++) { e.data1 = (uint8_t)(60 + i); CHECK(midi_ring_push(r, &e), "push"); }
    CHECK(midi_ring_push(r, &e) == 0, "push on full fails");
    int ok = 1;
    for (int i = 0; i < CAP; i++) { if (!midi_ring_pop(r, &o) || o.data1 != 60 + i) ok = 0; }
    CHECK(ok, "events come out in FIFO order");
}

static void test_no_drops_rapid(void)
{
    printf("- rapid key presses produce no dropped events\n");
    /* 16th notes at 180 BPM = 12 notes/s; simulate a few seconds: 64 notes,
     * each a Note On + Note Off, under running status, with clock bytes. */
    enum { NOTES = 64, CAP = 256 };
    unsigned char store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];
    midi_ring_t *r = (midi_ring_t *)store;
    midi_ring_init(r, CAP);
    midi_parser_t p; midi_parser_init(&p);

    int pushed = 0;
    for (int i = 0; i < NOTES; i++) {
        uint8_t seq[7] = { 0x90, (uint8_t)(48 + (i % 24)), 100,
                           0xF8, (uint8_t)(48 + (i % 24)), 0, 0xF8 };
        for (int b = 0; b < 7; b++) {
            midi_event_t ev;
            if (midi_parse_byte(&p, seq[b], &ev) && midi_ring_push(r, &ev)) pushed++;
        }
    }
    int popped = 0; midi_event_t o;
    while (midi_ring_pop(r, &o)) popped++;
    CHECK(pushed == NOTES * 2, "every Note On/Off was produced (no parser drop)");
    CHECK(popped == pushed, "every event made it through the ring (no drop)");
}

int main(void)
{
    printf("=== Tessera MIDI tests (issue #31) ===\n");
    test_basic();
    test_velocity0_off();
    test_running_status();
    test_realtime_interleave();
    test_garbage();
    test_ring();
    test_no_drops_rapid();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
