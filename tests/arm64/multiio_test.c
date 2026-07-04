/* tests/arm64/multiio_test.c - host unit tests for multi-channel audio I/O
 * (Theme H, issue #132).
 *
 * Build/run via:  make test-arm-multiio
 */

#include "multiio.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define NF 4

static void test_interleave(void)
{
    printf("- interleave / de-interleave round-trip (4 channels)\n");
    /* 4-channel interleaved: frame f, channel c = 100*c + f. */
    int16_t inter[NF * 4];
    for (int f = 0; f < NF; f++)
        for (int c = 0; c < 4; c++)
            inter[f * 4 + c] = (int16_t)(100 * c + f);

    int16_t p0[NF], p1[NF], p2[NF], p3[NF];
    int16_t *planes[4] = { p0, p1, p2, p3 };
    io_deinterleave(inter, NF, 4, planes);
    CHECK(p0[0] == 0 && p0[3] == 3, "channel 0 plane extracted");
    CHECK(p2[0] == 200 && p2[3] == 203, "channel 2 plane extracted");

    int16_t back[NF * 4];
    io_interleave(planes, NF, 4, back);
    int ok = 1;
    for (int i = 0; i < NF * 4; i++) if (back[i] != inter[i]) ok = 0;
    CHECK(ok, "re-interleave reproduces the original buffer");
}

static void test_identity_routing(void)
{
    printf("- default identity routing (4-in / 4-out)\n");
    io_config_t cfg;
    io_config_init(&cfg, 4, 4);
    CHECK(cfg.out_src[0] == 0 && cfg.out_src[3] == 3, "outputs map to matching graph channels");
    CHECK(cfg.in_src[2] == 2, "inputs map to matching device channels");

    /* Uneven config: 2-in, 6-out -> extra outputs are silence. */
    io_config_init(&cfg, 2, 6);
    CHECK(cfg.out_src[1] == 1 && cfg.out_src[5] == 5, "identity up to 6 outputs");
    CHECK(cfg.in_src[0] == 0 && cfg.in_src[1] == 1, "two inputs mapped");
    CHECK(cfg.in_src[2] == IO_SILENCE, "graph input 2 has no device input -> silence");
}

static void test_playback_routing(void)
{
    printf("- playback routing: duplicate, swap, and silence\n");
    io_config_t cfg;
    io_config_init(&cfg, 0, 4);            /* 4 output channels */

    int16_t g0[NF], g1[NF];
    for (int f = 0; f < NF; f++) { g0[f] = (int16_t)(10 + f); g1[f] = (int16_t)(20 + f); }
    int16_t *gout[2] = { g0, g1 };

    /* Route: dev0<-g0, dev1<-g0 (duplicate mono), dev2<-g1, dev3<-silence. */
    io_route_out(&cfg, 0, 0);
    io_route_out(&cfg, 1, 0);
    io_route_out(&cfg, 2, 1);
    io_route_out(&cfg, 3, IO_SILENCE);

    int16_t dev[NF * 4];
    io_playback(&cfg, gout, 2, NF, dev);
    CHECK(dev[0] == 10 && dev[1] == 10, "device 0 and 1 both carry graph channel 0");
    CHECK(dev[2] == 20, "device 2 carries graph channel 1");
    CHECK(dev[3] == 0, "device 3 is silent");
    /* Second frame. */
    CHECK(dev[4] == 11 && dev[6] == 21 && dev[7] == 0, "routing holds per frame");

    /* A route to a missing graph channel outputs silence, not garbage. */
    io_route_out(&cfg, 2, 5);              /* only 2 graph channels supplied */
    io_playback(&cfg, gout, 2, NF, dev);
    CHECK(dev[2] == 0, "route to an absent graph channel -> silence");
}

static void test_capture_routing(void)
{
    printf("- capture routing: swap a stereo pair, mute a channel\n");
    io_config_t cfg;
    io_config_init(&cfg, 2, 0);            /* 2 input channels */

    int16_t dev[NF * 2];
    for (int f = 0; f < NF; f++) { dev[f * 2] = (int16_t)(f); dev[f * 2 + 1] = (int16_t)(50 + f); }

    /* Swap L/R into the graph. */
    io_route_in(&cfg, 0, 1);
    io_route_in(&cfg, 1, 0);
    int16_t g0[NF], g1[NF];
    int16_t *gin[2] = { g0, g1 };
    io_capture(&cfg, dev, NF, gin, 2);
    CHECK(g0[0] == 50 && g1[0] == 0, "graph channels are the swapped device channels");

    /* Route graph channel 1 to silence. */
    io_route_in(&cfg, 1, IO_SILENCE);
    io_capture(&cfg, dev, NF, gin, 2);
    CHECK(g1[0] == 0 && g1[3] == 0, "muted graph channel is silent");
    CHECK(g0[3] == 53, "the other channel still carries audio");
}

static void test_bad_routes(void)
{
    printf("- routing bounds are enforced\n");
    io_config_t cfg;
    io_config_init(&cfg, 2, 2);
    CHECK(io_route_out(&cfg, 5, 0) == -1, "routing a nonexistent device output fails");
    CHECK(io_route_in(&cfg, 0, 9) == -1, "routing from a nonexistent device input fails");
    CHECK(io_route_out(&cfg, 0, IO_SILENCE) == 0, "routing an output to silence is allowed");
}

int main(void)
{
    printf("=== Tessera multi-channel I/O tests (Theme H, #132) ===\n");
    test_interleave();
    test_identity_routing();
    test_playback_routing();
    test_capture_routing();
    test_bad_routes();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
