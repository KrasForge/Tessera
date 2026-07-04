/* tests/arm64/sampler_test.c - host unit tests for the streaming sampler
 * (Theme M15, issue #165).
 *
 * Build/run via:  make test-arm-sampler
 */

#include "tessera.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_unity_pitch(void)
{
    printf("- unity pitch reproduces the source stream\n");
    float ring[64];
    tessera_sampler_t s;
    tessera_sampler_init(&s, ring, 64);

    float src[32];
    for (int i = 0; i < 32; i++) src[i] = (float)i;
    tessera_sampler_push(&s, src, 32);

    /* At unity pitch, output[k] == src[k] until the last sample (needs i+1). */
    int ok = 1;
    for (int k = 0; k < 31; k++) {
        float y = tessera_sampler_process(&s);
        if (y != src[k]) ok = 0;
    }
    CHECK(ok, "output equals the pushed samples");
}

static void test_half_pitch_interpolates(void)
{
    printf("- half pitch interpolates midpoints\n");
    float ring[64];
    tessera_sampler_t s;
    tessera_sampler_init(&s, ring, 64);
    tessera_sampler_set_pitch(&s, 0.5f);

    float src[8] = { 0, 10, 20, 30, 40, 50, 60, 70 };
    tessera_sampler_push(&s, src, 8);

    /* 0, 5, 10, 15, 20, ... */
    float expect[6] = { 0, 5, 10, 15, 20, 25 };
    int ok = 1;
    for (int k = 0; k < 6; k++) {
        float y = tessera_sampler_process(&s);
        if (fabsf(y - expect[k]) > 1e-4f) ok = 0;
    }
    CHECK(ok, "half-speed output is the interpolated stream");
}

static void test_octave_up(void)
{
    printf("- 2x pitch steps two source samples per output\n");
    float ring[64];
    tessera_sampler_t s;
    tessera_sampler_init(&s, ring, 64);
    tessera_sampler_set_pitch(&s, 2.0f);
    float src[16];
    for (int i = 0; i < 16; i++) src[i] = (float)(i * 10);
    tessera_sampler_push(&s, src, 16);
    CHECK(tessera_sampler_process(&s) == 0.0f,   "out0 = src[0]");
    CHECK(tessera_sampler_process(&s) == 20.0f,  "out1 = src[2]");
    CHECK(tessera_sampler_process(&s) == 40.0f,  "out2 = src[4]");
}

static void test_bounded_memory_and_loop(void)
{
    printf("- a long looped stream stays within the fixed ring\n");
    enum { CAP = 128, LOOP_LEN = 50, TOTAL_OUT = 5000 };
    float ring[CAP];
    tessera_sampler_t s;
    tessera_sampler_init(&s, ring, CAP);

    /* Host loop: the "file" is samples 0..LOOP_LEN, replayed forever. The host
     * feeds a monotonic stream by re-reading the loop region, keeping the ring
     * ahead of the play cursor. */
    uint64_t fed = 0;
    float out[TOTAL_OUT];
    int max_window = 0;
    for (int k = 0; k < TOTAL_OUT; k++) {
        /* Refill so the ring stays ahead (off-audio-path in reality). */
        uint32_t room = tessera_sampler_headroom(&s);
        if (room > 0) {
            float chunk[CAP];
            uint32_t take = room;
            for (uint32_t j = 0; j < take; j++) {
                chunk[j] = (float)((fed + j) % LOOP_LEN);   /* looped content */
            }
            tessera_sampler_push(&s, chunk, take);
            fed += take;
        }
        out[k] = tessera_sampler_process(&s);
        int window = (int)(fed - tessera_sampler_pos(&s));
        if (window > max_window) max_window = window;
    }

    CHECK(max_window <= CAP, "the buffered window never exceeds the ring capacity");
    /* The output is the looped sawtooth: out[k] == k % LOOP_LEN at unity pitch. */
    int ok = 1;
    for (int k = 0; k < TOTAL_OUT - 1; k++)
        if (fabsf(out[k] - (float)(k % LOOP_LEN)) > 1e-3f) { ok = 0; break; }
    CHECK(ok, "the looped stream plays back seamlessly (0..49 repeating)");
}

static void test_underrun_is_silent(void)
{
    printf("- a refill stall produces silence and holds position, not a stall\n");
    float ring[64];
    tessera_sampler_t s;
    tessera_sampler_init(&s, ring, 64);
    float src[4] = { 1, 2, 3, 4 };
    tessera_sampler_push(&s, src, 4);

    /* Consume the 3 interpolable samples, then starve it. */
    (void)tessera_sampler_process(&s);   /* src[0] */
    (void)tessera_sampler_process(&s);   /* src[1] */
    (void)tessera_sampler_process(&s);   /* src[2] */
    uint64_t pos_before = tessera_sampler_pos(&s);
    CHECK(tessera_sampler_process(&s) == 0.0f, "underrun returns silence");
    CHECK(tessera_sampler_pos(&s) == pos_before, "play cursor holds at the starve point");

    /* Feed more and it resumes exactly where it stopped. */
    float more[4] = { 5, 6, 7, 8 };
    tessera_sampler_push(&s, more, 4);
    CHECK(tessera_sampler_process(&s) == 4.0f, "resumes at src[3] once fed");
}

int main(void)
{
    printf("=== Tessera streaming-sampler tests (M15, #165) ===\n");
    test_unity_pitch();
    test_half_pitch_interpolates();
    test_octave_up();
    test_bounded_memory_and_loop();
    test_underrun_is_silent();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
