/* tests/arm64/bank_test.c - host unit tests for patch banks + MIDI Program
 * Change -> patch load (Theme E, issue #122).
 *
 * Build/run via:  make test-arm-bank
 */

#include "bank.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static midi_event_t cc(uint8_t num, uint8_t val)
{
    midi_event_t e = { MIDI_CC, 0, num, val, INPUT_SRC_MIDI };
    return e;
}
static midi_event_t pc(uint8_t program)
{
    midi_event_t e = { MIDI_PROGRAM, 0, program, 0, INPUT_SRC_MIDI };
    return e;
}

static void test_resolve(void)
{
    printf("- populate banks and resolve (bank, program) -> path\n");
    bank_set_t bs; bank_set_init(&bs);
    int a = bank_add(&bs, "Factory");
    int b = bank_add(&bs, "User");
    CHECK(a == 0 && b == 1 && bs.n_banks == 2, "two banks added");

    bank_set_program(&bs, a, 0, "/sd/factory/clean.patch");
    bank_set_program(&bs, a, 5, "/sd/factory/lead.patch");
    bank_set_program(&bs, b, 0, "/sd/user/mine.patch");

    char path[PATCH_PATH_MAX];
    CHECK(bank_resolve(&bs, a, 0, path) && strcmp(path, "/sd/factory/clean.patch") == 0,
          "factory program 0 resolves");
    CHECK(bank_resolve(&bs, a, 5, path) && strcmp(path, "/sd/factory/lead.patch") == 0,
          "factory program 5 resolves");
    CHECK(bank_resolve(&bs, b, 0, path) && strcmp(path, "/sd/user/mine.patch") == 0,
          "user program 0 resolves");
    CHECK(bank_resolve(&bs, a, 1, path) == 0, "an empty slot does not resolve");
    CHECK(bank_resolve(&bs, 9, 0, path) == 0, "an out-of-range bank does not resolve");
}

static void test_program_change(void)
{
    printf("- a Program Change loads the resolved patch in the current bank\n");
    bank_set_t bs; bank_set_init(&bs);
    bank_add(&bs, "Factory");
    bank_set_program(&bs, 0, 3, "/sd/factory/three.patch");
    bank_set_program(&bs, 0, 7, "/sd/factory/seven.patch");

    char path[PATCH_PATH_MAX];
    midi_event_t ev = pc(3);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/factory/three.patch") == 0,
          "PC 3 -> three.patch");
    ev = pc(7);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/factory/seven.patch") == 0,
          "PC 7 -> seven.patch");
    ev = pc(4);
    CHECK(bank_midi(&bs, &ev, path) == 0, "PC to an empty program loads nothing");

    /* A non-PC/CC event is ignored. */
    midi_event_t note = { MIDI_NOTE_ON, 0, 60, 100, INPUT_SRC_MIDI };
    CHECK(bank_midi(&bs, &note, path) == 0, "a note event triggers no load");
}

static void test_bank_select(void)
{
    printf("- Bank Select (CC0/CC32) latches; the next PC uses it\n");
    bank_set_t bs; bank_set_init(&bs);
    bank_add(&bs, "Bank0");   /* index 0 */
    bank_add(&bs, "Bank1");   /* index 1 */
    bank_set_program(&bs, 0, 0, "/sd/b0/p0.patch");
    bank_set_program(&bs, 1, 0, "/sd/b1/p0.patch");

    char path[PATCH_PATH_MAX];
    /* Default bank is 0. */
    midi_event_t ev = pc(0);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/b0/p0.patch") == 0,
          "PC 0 in default bank -> bank 0");

    /* Select bank 1 (MSB 0, LSB 1) then PC 0. */
    midi_event_t msb = cc(0, 0), lsb = cc(32, 1);
    CHECK(bank_midi(&bs, &msb, path) == 0, "Bank Select MSB latches, no load");
    CHECK(bank_midi(&bs, &lsb, path) == 0, "Bank Select LSB latches, no load");
    ev = pc(0);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/b1/p0.patch") == 0,
          "PC 0 after Bank Select 1 -> bank 1");

    /* Bank sticks for subsequent PCs without another Bank Select. */
    ev = pc(0);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/b1/p0.patch") == 0,
          "the selected bank persists across PCs");

    /* An out-of-range Bank Select is ignored (current bank unchanged). */
    midi_event_t bad = cc(0, 5);
    bank_midi(&bs, &bad, path);
    ev = pc(0);
    CHECK(bank_midi(&bs, &ev, path) && strcmp(path, "/sd/b1/p0.patch") == 0,
          "an out-of-range bank select is ignored");
}

int main(void)
{
    printf("=== Tessera patch-bank / program-change tests (Theme E, #122) ===\n");
    test_resolve();
    test_program_change();
    test_bank_select();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
