/* tests/arm64/hot_reload_test.c - host unit tests for plugin hot-reload
 * (Theme A: reliability).
 *
 * The reload state machine sequences a live plugin swap so the audio never
 * drops: the running generation produces every block until the new one is
 * loaded and initialised, the swap commits at exactly one boundary, a failed
 * load leaves the old version untouched, and generations advance monotonically.
 * The logic is pure, so it is checked here for exact behaviour.
 *
 * Build/run via:  make test-arm-hot-reload
 */

#include "hot_reload.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_init(void)
{
    printf("- init: generation 0 running, nothing pending\n");
    hr_state_t s; hr_init(&s);
    CHECK(s.active == 0u, "active generation is 0");
    CHECK(s.pending == HR_NONE, "no pending generation");
    CHECK(hr_phase(&s) == HR_STEADY, "phase is steady");
    uint32_t retired;
    CHECK(hr_next(&s, &retired) == 0u && retired == HR_NONE, "steady blocks run gen 0, no retire");
}

static void test_prepare_keeps_old_running(void)
{
    printf("- while a new version loads, the old one keeps producing every block\n");
    hr_state_t s; hr_init(&s);
    CHECK(hr_prepare(&s) == 1, "prepare accepted from steady");
    CHECK(hr_phase(&s) == HR_PREPARING, "phase is preparing");
    CHECK(s.pending == 1u, "pending generation is 1");
    uint32_t retired;
    int stayed = 1;
    for (int b = 0; b < 5; b++)
        if (hr_next(&s, &retired) != 0u || retired != HR_NONE) stayed = 0;
    CHECK(stayed, "5 blocks during load all run gen 0 with no swap (no dropout)");
    CHECK(s.swaps == 0u, "no swap committed while still loading");
}

static void test_prepare_is_exclusive(void)
{
    printf("- only one reload in flight at a time\n");
    hr_state_t s; hr_init(&s);
    hr_prepare(&s);
    CHECK(hr_prepare(&s) == 0, "second prepare rejected while preparing");
    hr_ready(&s, 1);
    CHECK(hr_prepare(&s) == 0, "prepare rejected while armed");
    CHECK(s.prepares == 1u, "only one prepare counted");
}

static void test_swap_commits_once(void)
{
    printf("- ready(ok) then one boundary swap retires the old generation exactly once\n");
    hr_state_t s; hr_init(&s);
    hr_prepare(&s);
    CHECK(hr_ready(&s, 1) == HR_ARMED, "ready(ok) arms the swap");
    uint32_t retired;
    uint32_t run = hr_next(&s, &retired);          /* the swap boundary */
    CHECK(run == 1u, "the swapped-in block runs gen 1");
    CHECK(retired == 0u, "gen 0 is retired at the swap");
    CHECK(s.swaps == 1u, "one swap counted");
    CHECK(hr_phase(&s) == HR_STEADY, "back to steady after the swap");
    CHECK(s.pending == HR_NONE, "nothing pending after the swap");
    /* subsequent blocks: steady on gen 1, no further retire */
    int steady = 1;
    for (int b = 0; b < 3; b++)
        if (hr_next(&s, &retired) != 1u || retired != HR_NONE) steady = 0;
    CHECK(steady, "later blocks run gen 1 with no further retire");
}

static void test_failed_load_rolls_back(void)
{
    printf("- a load that fails leaves the running version in place (no dropout)\n");
    hr_state_t s; hr_init(&s);
    hr_prepare(&s);
    CHECK(hr_ready(&s, 0) == HR_STEADY, "ready(fail) returns to steady");
    CHECK(s.aborts == 1u, "one abort counted");
    CHECK(s.pending == HR_NONE, "pending cleared on abort");
    uint32_t retired;
    int stayed = 1;
    for (int b = 0; b < 4; b++)
        if (hr_next(&s, &retired) != 0u || retired != HR_NONE) stayed = 0;
    CHECK(stayed, "gen 0 keeps producing after the failed load");
    CHECK(s.swaps == 0u, "no swap after a failed load");
}

static void test_generations_monotonic(void)
{
    printf("- repeated reloads advance the generation monotonically, never revisiting\n");
    hr_state_t s; hr_init(&s);
    uint32_t retired, seen_prev = 0;
    int mono = 1, retire_ok = 1;
    for (uint32_t g = 1; g <= 4; g++) {
        hr_prepare(&s);
        hr_ready(&s, 1);
        uint32_t run = hr_next(&s, &retired);
        if (run != g) mono = 0;                    /* active becomes 1,2,3,4 */
        if (retired != seen_prev) retire_ok = 0;   /* retires g-1 each time  */
        seen_prev = run;
    }
    CHECK(mono, "active generation is 1,2,3,4 across four reloads");
    CHECK(retire_ok, "each reload retires exactly the previous generation");
    CHECK(s.swaps == 4u, "four swaps counted");
}

static void test_ready_without_prepare_ignored(void)
{
    printf("- ready() with nothing preparing is a no-op\n");
    hr_state_t s; hr_init(&s);
    CHECK(hr_ready(&s, 1) == HR_STEADY, "ready(ok) ignored while steady");
    uint32_t retired;
    CHECK(hr_next(&s, &retired) == 0u && retired == HR_NONE, "no spurious swap");
    CHECK(s.swaps == 0u, "no swap counted");
}

int main(void)
{
    printf("=== Tessera plugin hot-reload tests (Theme A) ===\n");
    test_init();
    test_prepare_keeps_old_running();
    test_prepare_is_exclusive();
    test_swap_commits_once();
    test_failed_load_rolls_back();
    test_generations_monotonic();
    test_ready_without_prepare_ignored();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
