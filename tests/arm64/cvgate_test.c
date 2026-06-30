/* tests/arm64/cvgate_test.c - host unit tests for CV/Gate input (Issue #32).
 *
 * Checks the MCP3208 command/decode, the 1V/octave pitch scaling at 0..5 V, the
 * gate-edge to note-event translation, and that CV events coexist with MIDI
 * events on one ring without interference.
 *
 * Build/run via:  make test-arm-cvgate
 */

#include "cvgate.h"
#include "midi.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* 12-bit ADC over 0..5 V: 4095/5 ~= 819 codes/volt, 0 V at code 0, C2 at 0 V. */
static const cv_cal_t CAL = { .code_0v = 0, .codes_per_volt = 819, .base_note = 36 };

static void test_mcp3208(void)
{
    printf("- MCP3208 command / decode\n");
    uint8_t tx[3];
    mcp3208_command(0, tx);
    CHECK(tx[0] == 0x06 && tx[1] == 0x00, "channel 0 command bytes");
    mcp3208_command(7, tx);
    CHECK(tx[0] == 0x07 && tx[1] == 0xC0, "channel 7 command bytes");

    uint8_t rx[3] = { 0x00, 0x0A, 0xBC };           /* 12-bit 0xABC */
    CHECK(mcp3208_decode(rx) == 0x0ABC, "decode assembles the 12-bit result");
    uint8_t rxhi[3] = { 0xFF, 0xFF, 0xFF };
    CHECK(mcp3208_decode(rxhi) == 0x0FFF, "decode masks to 12 bits");
}

static void test_scaling(void)
{
    printf("- 1V/octave pitch scaling at 0..5 V\n");
    static const uint16_t codes[6] = { 0, 819, 1638, 2457, 3276, 4095 };
    static const uint8_t  notes[6] = { 36, 48, 60, 72, 84, 96 };
    int ok = 1;
    for (int v = 0; v <= 5; v++) {
        uint8_t n = cv_to_note(codes[v], &CAL);
        printf("    %dV: code=%u -> note=%u (expect %u)\n", v, codes[v], n, notes[v]);
        int diff = (int)n - (int)notes[v];
        if (diff < -1 || diff > 1) ok = 0;          /* within one semitone */
    }
    CHECK(ok, "each test voltage reads within one semitone");

    /* Noisy code near 1 V still rounds within a semitone. */
    uint8_t n = cv_to_note(819 + 40, &CAL);
    CHECK(n >= 47 && n <= 49, "slightly-off 1 V code stays within a semitone");

    /* Clamping. */
    CHECK(cv_to_note(0, &CAL) == 36, "0 V clamps fine at base");
    cv_cal_t hi = { 0, 819, 120 };
    CHECK(cv_to_note(4095, &hi) == 127, "out-of-range high note clamps to 127");
}

static void test_gate_edges(void)
{
    printf("- gate edges -> note events (source = CV)\n");
    enum { CAP = 16 };
    unsigned char store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];
    midi_ring_t *ring = (midi_ring_t *)store;
    midi_ring_init(ring, CAP);

    cvgate_t cg;
    cvgate_init(&cg, &CAL, 0);

    CHECK(cvgate_update(&cg, 0, 1638, ring) == 0, "no edge -> no event");
    CHECK(cvgate_update(&cg, 1, 1638, ring) == 1, "rising edge emits");   /* 2 V */
    CHECK(cvgate_update(&cg, 1, 1638, ring) == 0, "held high -> no event");
    CHECK(cvgate_update(&cg, 0, 1638, ring) == 1, "falling edge emits");

    midi_event_t e;
    midi_ring_pop(ring, &e);
    CHECK(e.type == MIDI_NOTE_ON && e.data1 == 60 && e.data2 == 127 &&
          e.source == INPUT_SRC_CV, "rising -> NOTE_ON note 60 (2 V), CV source");
    midi_ring_pop(ring, &e);
    CHECK(e.type == MIDI_NOTE_OFF && e.source == INPUT_SRC_CV,
          "falling -> NOTE_OFF, CV source");
}

static void test_coexistence(void)
{
    printf("- CV and MIDI events share one ring without interference\n");
    enum { CAP = 16 };
    unsigned char store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];
    midi_ring_t *ring = (midi_ring_t *)store;
    midi_ring_init(ring, CAP);

    /* A MIDI Note On via the parser. */
    midi_parser_t p; midi_parser_init(&p);
    uint8_t bytes[] = { 0x90, 72, 100 };
    for (int i = 0; i < 3; i++) {
        midi_event_t e;
        if (midi_parse_byte(&p, bytes[i], &e)) midi_ring_push(ring, &e);
    }
    /* A CV gate rising. */
    cvgate_t cg; cvgate_init(&cg, &CAL, 1);
    cvgate_update(&cg, 1, 819, ring);          /* 1 V -> note 48 */
    /* Another MIDI Note Off. */
    uint8_t off[] = { 0x80, 72, 0 };
    for (int i = 0; i < 3; i++) {
        midi_event_t e;
        if (midi_parse_byte(&p, off[i], &e)) midi_ring_push(ring, &e);
    }

    midi_event_t e;
    CHECK(midi_ring_pop(ring, &e) && e.source == INPUT_SRC_MIDI &&
          e.type == MIDI_NOTE_ON && e.data1 == 72, "1st: MIDI Note On intact");
    CHECK(midi_ring_pop(ring, &e) && e.source == INPUT_SRC_CV &&
          e.type == MIDI_NOTE_ON && e.data1 == 48, "2nd: CV Note On intact");
    CHECK(midi_ring_pop(ring, &e) && e.source == INPUT_SRC_MIDI &&
          e.type == MIDI_NOTE_OFF && e.data1 == 72, "3rd: MIDI Note Off intact");
    CHECK(midi_ring_count(ring) == 0, "ring drained; sources did not interfere");
}

int main(void)
{
    printf("=== Tessera CV/Gate tests (issue #32) ===\n");
    test_mcp3208();
    test_scaling();
    test_gate_edges();
    test_coexistence();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
