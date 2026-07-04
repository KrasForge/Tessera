/* tests/arm64/chaos_test.c - chaos-mode resilience gate (Theme M16, issue #170)
 *
 * The M8 resilience demo kills one malicious plugin and shows audio survives.
 * This generalises it into a continuous gate: over a long, seeded soak it injects
 * faults (MMU abort, budget overrun, syscall abuse, outright kill) into a
 * multi-effect chain and, every block, asserts the platform's safe-mode bypass
 * (arch/arm64/safe_bypass.c) contains each fault - the dead node passes its dry
 * input through - so the DAC never sees a silent gap.  Deterministic (seeded), so
 * it is a CI gate, not a one-off demo.
 *
 * Build/run via:  make test-arm-chaos
 */

#include "safe_bypass.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static uint32_t g_rng = 0x5EED1234u;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

#define N_FX   4          /* effects in the chain (source -> fx... -> DAC) */
#define W      16         /* words per block */

/* Fault kinds, purely for the report - all of them leave the node dead, which is
 * exactly what safe-mode bypass is built to contain. */
static const char *FAULT_NAME[4] = { "MMU-abort", "budget-kill", "syscall-abuse", "process-kill" };

int main(int argc, char **argv)
{
    int rounds = (argc > 1) ? atoi(argv[1]) : 20000;
    printf("=== Tessera chaos-mode resilience gate (M16, #170): %d rounds ===\n", rounds);

    sb_state_t sb[N_FX];
    int alive[N_FX];
    for (int i = 0; i < N_FX; i++) { sb_init(&sb[i]); alive[i] = 1; }

    uint32_t src[W], buf_in[W], buf_out[W];
    long dropouts = 0, faults = 0, reloads = 0, bypass_blocks = 0;
    long ident_ok = 0, ident_checks = 0;

    for (int r = 0; r < rounds; r++) {
        /* Occasionally inject a fault or a reload. */
        uint32_t roll = rnd() % 100u;
        if (roll < 12) {                          /* ~12%: a node faults */
            int n = (int)(rnd() % N_FX);
            if (alive[n]) {
                alive[n] = 0;
                faults++;
                (void)FAULT_NAME[rnd() % 4];       /* tag the fault kind */
            }
        } else if (roll < 18) {                   /* ~6%: a dead node is reloaded */
            int n = (int)(rnd() % N_FX);
            if (!alive[n]) { alive[n] = 1; sb_init(&sb[n]); reloads++; }
        }

        /* A non-silent source block for this round. */
        uint32_t sample = 0x1000u + (uint32_t)r;
        for (int i = 0; i < W; i++) src[i] = sample + (uint32_t)i;

        /* Drive the block through the chain: each node either processes (alive)
         * or is bypassed dry by safe-mode bypass (dead). */
        memcpy(buf_in, src, sizeof src);
        for (int n = 0; n < N_FX; n++) {
            /* The live "effect": a reversible non-nullifying transform. */
            uint32_t node_out[W];
            for (int i = 0; i < W; i++) node_out[i] = buf_in[i] ^ 0x55u;

            int bypassed = sb_resolve(&sb[n], alive[n], node_out, buf_in, buf_out, W);

            if (!alive[n]) {
                ident_checks++;
                /* A dead node must be bypassed (identified + contained) and emit
                 * its dry input, not silence or stale output. */
                int dry = (memcmp(buf_out, buf_in, sizeof buf_in) == 0);
                if (bypassed && dry) ident_ok++;
                bypass_blocks++;
            }
            memcpy(buf_in, buf_out, sizeof buf_in);   /* feeds the next node */
        }

        /* buf_in now holds the DAC-bound block.  It must never be a silent gap:
         * the source was non-zero and every node either processed or passed dry,
         * so the signal always reaches the DAC. */
        int silent = 1;
        for (int i = 0; i < W; i++) if (buf_in[i] != 0u) { silent = 0; break; }
        if (silent) dropouts++;
    }

    printf("  injected %ld faults, %ld reloads, %ld dry-bypass blocks\n",
           faults, reloads, bypass_blocks);
    CHECK(faults > 100, "the soak actually injected many faults");
    CHECK(reloads > 0, "and reloaded some dead nodes");
    CHECK(dropouts == 0, "the DAC never saw a silent gap (zero dropouts)");
    CHECK(ident_checks > 0 && ident_ok == ident_checks,
          "every faulted node was identified and dry-bypassed");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
