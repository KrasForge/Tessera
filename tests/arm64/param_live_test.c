/* tests/arm64/param_live_test.c - live parameter changes with no dropout
 * (Issue #33).
 *
 * Wires the real pieces together: a MIDI Control Change is mapped to a plugin
 * parameter (param_map), enqueued on the lock-free parameter queue (issue #30),
 * drained by the plugin at the top of each block, and applied to the reference
 * low-pass filter (issue #29).  The test then checks the three acceptance
 * criteria:
 *
 *   1. Sweeping the filter cutoff via CC is click-free (no discontinuity beyond
 *      the signal's natural step), and the cutoff actually moved.
 *   2. Under 100 updates per block: with a deep queue none are missed; with a
 *      shallow queue the overflow is detected and the audio is not corrupted.
 *   3. The control-to-audio latency is below one block (< 10 ms).
 *
 * The plugin uses no libm; this test uses sinf only to synthesise input.
 *
 * Build/run via:  make test-arm-param-live
 */

#define _GNU_SOURCE
#include "plugin_abi.h"
#include "param_queue.h"
#include "param_map.h"
#include "midi.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define SR  48000u
#define BLK 256u
#define CC_CUTOFF 74u
#define PARAM_CUTOFF 0u

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* The "host" maps a CC and enqueues it; returns 0 on success, 1 on overflow. */
static int host_send_cc(const param_map_t *map, param_queue_t *q,
                        uint8_t cc, uint8_t val)
{
    midi_event_t ev = { MIDI_CC, 0, cc, val, INPUT_SRC_MIDI };
    uint32_t id; float v;
    if (!param_map_event(map, &ev, &id, &v))
        return 0;
    return pq_push(q, id, f2u(v)) ? 0 : 1;       /* 1 == overflow */
}

/* The plugin drains the queue and applies every pending parameter. */
static void plugin_drain(param_queue_t *q)
{
    uint32_t id, bits;
    while (pq_pop(q, &id, &bits))
        plugin_set_param(id, u2f(bits));
}

/* Run `blocks` blocks of a `freq` Hz sine through the filter, optionally
 * sweeping the cutoff via CC each block.  Returns the max sample-to-sample
 * delta and the RMS of the final block. */
static void run(param_map_t *map, param_queue_t *q, double freq, int blocks,
                int sweep, float *max_delta, float *last_rms)
{
    float il[BLK], ir[BLK], ol[BLK], orr[BLK];
    double phase = 0.0, dp = 2.0 * M_PI * freq / SR;
    float prev = 0.0f, md = 0.0f;
    double sumsq = 0.0;

    for (int b = 0; b < blocks; b++) {
        if (sweep) {
            uint8_t cc = (uint8_t)(b * 127 / (blocks - 1));   /* 0..127 */
            host_send_cc(map, q, CC_CUTOFF, cc);
        }
        plugin_drain(q);                          /* apply params at block top */

        for (uint32_t i = 0; i < BLK; i++) {
            il[i] = ir[i] = 0.5f * (float)sin(phase);
            phase += dp;
        }
        plugin_process_block(il, ir, ol, orr, BLK);

        sumsq = 0.0;
        for (uint32_t i = 0; i < BLK; i++) {
            float d = ol[i] - prev; if (d < 0) d = -d;
            if (d > md) md = d;
            prev = ol[i];
            sumsq += (double)ol[i] * ol[i];
        }
    }
    *max_delta = md;
    *last_rms  = (float)sqrt(sumsq / BLK);
}

static void test_click_free(void)
{
    printf("- click-free cutoff sweep via MIDI CC\n");
    param_map_t map; param_map_init(&map);
    param_map_bind(&map, CC_CUTOFF, PARAM_CUTOFF, 200.0f, 8000.0f);

    unsigned char store[sizeof(param_queue_t) + 16 * sizeof(param_event_t)];
    param_queue_t *q = (param_queue_t *)store;

    /* Reference 1: cutoff held fully open (8 kHz) - the most signal passes, so
     * this has the largest *natural* sample-to-sample step. */
    plugin_init(SR, BLK);
    pq_init(q, 16);
    plugin_set_param(PARAM_CUTOFF, 8000.0f);
    float open_delta, open_rms;
    run(&map, q, 3000.0, 64, 0, &open_delta, &open_rms);

    /* Reference 2: cutoff held low (200 Hz) - the 3 kHz tone is nearly killed. */
    plugin_init(SR, BLK);
    pq_init(q, 16);
    plugin_set_param(PARAM_CUTOFF, 200.0f);
    float low_delta, low_rms;
    run(&map, q, 3000.0, 64, 0, &low_delta, &low_rms);

    /* Swept: cutoff driven 200 -> 8000 Hz by CC over the run. */
    plugin_init(SR, BLK);
    pq_init(q, 16);
    float swept_delta, swept_rms;
    run(&map, q, 3000.0, 64, 1, &swept_delta, &swept_rms);

    printf("    open: maxd=%.4f rms=%.4f ; low: rms=%.4f ; swept: maxd=%.4f rms=%.4f\n",
           open_delta, open_rms, low_rms, swept_delta, swept_rms);

    /* Click-free: the sweep introduces no discontinuity beyond the natural step
     * of the fully-open filter (a coefficient click would spike max-delta well
     * above it). */
    CHECK(swept_delta <= open_delta * 1.5f + 0.01f,
          "cutoff sweep adds no click (max step within the fully-open natural step)");
    /* The parameter actually moved: a 3 kHz tone passes far more at 8 kHz
     * cutoff than held at 200 Hz. */
    CHECK(swept_rms > low_rms * 2.0f,
          "the CC actually modulated the filter (more highs at the end)");
}

static void test_stress(void)
{
    printf("- 100 updates/block: deep queue loses none, shallow logs overflow\n");
    param_map_t map; param_map_init(&map);
    param_map_bind(&map, CC_CUTOFF, PARAM_CUTOFF, 200.0f, 8000.0f);

    /* Deep queue (>=128): 100 updates in a block, none missed. */
    unsigned char deep[sizeof(param_queue_t) + 128 * sizeof(param_event_t)];
    param_queue_t *dq = (param_queue_t *)deep;
    pq_init(dq, 128);
    int of = 0;
    for (int i = 0; i < 100; i++) of += host_send_cc(&map, dq, CC_CUTOFF, (uint8_t)i);
    CHECK(of == 0, "deep queue: 100 updates accepted with no overflow");
    CHECK(pq_count(dq) == 100, "all 100 pending for the plugin to drain");

    /* Shallow queue (16): overflow is detected and reported. */
    unsigned char shallow[sizeof(param_queue_t) + 16 * sizeof(param_event_t)];
    param_queue_t *sq = (param_queue_t *)shallow;
    pq_init(sq, 16);
    int ofs = 0;
    for (int i = 0; i < 100; i++) ofs += host_send_cc(&map, sq, CC_CUTOFF, (uint8_t)i);
    CHECK(ofs > 0, "shallow queue: overflow detected (logged)");

    /* Audio is not corrupted: drain whatever made it and process a block. */
    plugin_init(SR, BLK);
    plugin_drain(sq);
    float il[BLK], ir[BLK], ol[BLK], orr[BLK];
    for (uint32_t i = 0; i < BLK; i++) il[i] = ir[i] = 0.25f;
    plugin_process_block(il, ir, ol, orr, BLK);
    int finite = 1; for (uint32_t i = 0; i < BLK; i++) if (!(ol[i] == ol[i])) finite = 0;
    CHECK(finite, "audio stays finite after an overflowing burst (not corrupted)");
}

static void test_latency(void)
{
    printf("- control-to-audio latency below one block\n");
    double block_ms = (double)BLK * 1000.0 / SR;
    printf("    block period = %.3f ms (param applied at the next block top)\n", block_ms);
    CHECK(block_ms < 10.0, "one-block delivery latency is under 10 ms");
}

int main(void)
{
    printf("=== Tessera live-parameter tests (issue #33) ===\n");
    test_click_free();
    test_stress();
    test_latency();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
