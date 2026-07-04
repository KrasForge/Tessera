/* tests/arm64/arp_test.c - host unit tests for the arpeggiator (Theme C, #116).
 *
 * The arp turns a held chord into a timed note stream locked to the transport.
 * Checked here: held-note bookkeeping, the up/down/up-down/random step patterns
 * at the expected tick boundaries, one event per step (not per block), and the
 * note-off behaviour when notes are released or the arp is disabled.
 *
 * Build/run via:  make test-arm-arp
 */

#include "arp.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define STEP 24   /* ticks per step (1/16 at TP_PPQ=96) */

/* Run one block at `tick`; return the note-on note (or 0), and set *offs. */
static uint8_t on_note(arp_t *a, int64_t tick, int *n_ev)
{
    arp_event_t ev[4];
    int n = arp_run(a, tick, ev, 4);
    *n_ev = n;
    for (int i = 0; i < n; i++) if (ev[i].on) return ev[i].note;
    return 0;
}

static void hold_chord(arp_t *a) { arp_note_on(a, 64); arp_note_on(a, 60); arp_note_on(a, 67); }

static void test_held_set(void)
{
    printf("- held notes kept sorted, de-duplicated\n");
    arp_t a; arp_init(&a, ARP_UP, STEP, 100);
    hold_chord(&a);
    arp_note_on(&a, 64);                       /* duplicate */
    CHECK(a.n_held == 3, "three unique notes held");
    CHECK(a.held[0] == 60 && a.held[1] == 64 && a.held[2] == 67, "sorted ascending");
    arp_note_off(&a, 64);
    CHECK(a.n_held == 2 && a.held[0] == 60 && a.held[1] == 67, "note-off removes and closes the gap");
}

static void test_up(void)
{
    printf("- UP mode cycles the chord ascending, one note per step\n");
    arp_t a; arp_init(&a, ARP_UP, STEP, 100); hold_chord(&a);
    int n;
    CHECK(on_note(&a, 0,  &n) == 60 && n == 1, "step 0 -> on 60 (no prior note-off)");
    CHECK(on_note(&a, 12, &n) == 0  && n == 0, "mid-step: no event");
    CHECK(on_note(&a, 24, &n) == 64 && n == 2, "step 1 -> off 60, on 64");
    CHECK(on_note(&a, 48, &n) == 67 && n == 2, "step 2 -> on 67");
    CHECK(on_note(&a, 72, &n) == 60 && n == 2, "step 3 wraps -> on 60");
}

static void test_down(void)
{
    printf("- DOWN mode cycles descending\n");
    arp_t a; arp_init(&a, ARP_DOWN, STEP, 100); hold_chord(&a);
    int n;
    CHECK(on_note(&a, 0,  &n) == 67, "step 0 -> on 67");
    CHECK(on_note(&a, 24, &n) == 64, "step 1 -> on 64");
    CHECK(on_note(&a, 48, &n) == 60, "step 2 -> on 60");
    CHECK(on_note(&a, 72, &n) == 67, "step 3 wraps -> on 67");
}

static void test_updown(void)
{
    printf("- UPDOWN mode ping-pongs without repeating the endpoints\n");
    arp_t a; arp_init(&a, ARP_UPDOWN, STEP, 100); hold_chord(&a);
    int n;
    uint8_t seq[7];
    for (int i = 0; i < 7; i++) seq[i] = on_note(&a, (int64_t)i * STEP, &n);
    CHECK(seq[0] == 60 && seq[1] == 64 && seq[2] == 67, "up: 60,64,67");
    CHECK(seq[3] == 64 && seq[4] == 60, "down: 64,60 (endpoints not repeated)");
    CHECK(seq[5] == 64 && seq[6] == 67, "period repeats: 64,67");
}

static void test_random_deterministic(void)
{
    printf("- RANDOM mode is deterministic and stays in range\n");
    arp_t a; arp_init(&a, ARP_RANDOM, STEP, 100); hold_chord(&a);
    arp_t b; arp_init(&b, ARP_RANDOM, STEP, 100); hold_chord(&b);
    int n; int in_range = 1, same = 1, varied = 0; uint8_t prev = 0;
    for (int i = 0; i < 12; i++) {
        uint8_t x = on_note(&a, (int64_t)i * STEP, &n);
        uint8_t y = on_note(&b, (int64_t)i * STEP, &n);
        if (x != y) same = 0;
        if (x != 60 && x != 64 && x != 67) in_range = 0;
        if (i > 0 && x != prev) varied = 1;
        prev = x;
    }
    CHECK(in_range, "every note is one of the held notes");
    CHECK(same, "two arps with the same steps produce the same sequence");
    CHECK(varied, "the sequence actually varies");
}

static void test_release_and_disable(void)
{
    printf("- releasing all notes / disabling stops the sounding note\n");
    arp_t a; arp_init(&a, ARP_UP, STEP, 100);
    arp_note_on(&a, 60);
    int n;
    CHECK(on_note(&a, 0, &n) == 60 && n == 1, "sounds 60");
    arp_note_off(&a, 60);                          /* all released */
    arp_event_t ev[4];
    n = arp_run(&a, 24, ev, 4);
    CHECK(n == 1 && ev[0].on == 0 && ev[0].note == 60, "note-off emitted when the chord clears");

    arp_t b; arp_init(&b, ARP_UP, STEP, 100); arp_note_on(&b, 72);
    on_note(&b, 0, &n);                             /* sounds 72 */
    arp_enable(&b, 0);
    n = arp_run(&b, 24, ev, 4);
    CHECK(n == 1 && ev[0].on == 0 && ev[0].note == 72, "disable silences the sounding note");
}

int main(void)
{
    printf("=== Tessera arpeggiator tests (Theme C, #116) ===\n");
    test_held_set();
    test_up();
    test_down();
    test_updown();
    test_random_deterministic();
    test_release_and_disable();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
