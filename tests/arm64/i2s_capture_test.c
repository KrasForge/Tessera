/* tests/arm64/i2s_capture_test.c - host unit tests for the I2S capture ring
 * (Issue #83).
 *
 * The capture ring is pure C, so the properties that matter for audio input -
 * bit-exact capture, correct wrap-around, drop-the-oldest overrun on a full
 * ring, and silence-on-underrun for the consumer - are validated on the host
 * against known patterns, plus the DMA control-block encoding for the RX
 * direction.
 *
 * Build/run via:  make test-arm-i2s-rx
 */

#include "i2s_capture.h"
#include "dma.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define FRAMES   4u                      /* small blocks to force wrap fast */
#define NBLOCKS  3u
#define SAMPLES  (FRAMES * 2u)

/* Fill `blk` with a recognisable pattern keyed by `seq`. */
static void make_block(int16_t *blk, int seq)
{
    for (uint32_t i = 0; i < SAMPLES; i++)
        blk[i] = (int16_t)(seq * 100 + (int)i);
}
static int block_is(const int16_t *blk, int seq)
{
    for (uint32_t i = 0; i < SAMPLES; i++)
        if (blk[i] != (int16_t)(seq * 100 + (int)i))
            return 0;
    return 1;
}

/* ---- bit-exact FIFO capture and wrap-around ---- */
static void test_fifo_wrap(void)
{
    printf("- capture: bit-exact FIFO order across wrap-around\n");
    int16_t store[NBLOCKS * SAMPLES];
    i2s_capture_t c;
    i2s_capture_init(&c, store, NBLOCKS, FRAMES);

    /* Produce and consume 12 blocks one at a time: the ring (3 slots) wraps
     * four times, and every block must come back bit-exact and in order. */
    int ok = 1;
    for (int seq = 0; seq < 12; seq++) {
        int16_t in[SAMPLES], out[SAMPLES];
        make_block(in, seq);
        i2s_capture_produce(&c, in);
        CHECK(i2s_capture_available(&c) == 1, "one block available after produce");
        if (!i2s_capture_consume(&c, out) || !block_is(out, seq))
            ok = 0;
    }
    CHECK(ok, "12 blocks captured bit-exact and in order across wrap");
    CHECK(c.produced == 12 && c.consumed == 12 && c.overruns == 0 &&
          c.underruns == 0, "counters: 12 in, 12 out, no over/underrun");
}

/* ---- overrun: full ring drops the oldest ---- */
static void test_overrun(void)
{
    printf("- overrun: a full ring drops the oldest, keeps the newest\n");
    int16_t store[NBLOCKS * SAMPLES];
    i2s_capture_t c;
    i2s_capture_init(&c, store, NBLOCKS, FRAMES);

    /* Fill the 3 slots, then produce two more without consuming: the source
     * cannot stall, so blocks 0 and 1 are dropped. */
    for (int seq = 0; seq < 5; seq++) {
        int16_t in[SAMPLES];
        make_block(in, seq);
        i2s_capture_produce(&c, in);
    }
    CHECK(i2s_capture_available(&c) == NBLOCKS, "ring stays full, not over-filled");
    CHECK(c.overruns == 2, "two oldest blocks dropped and counted");

    /* What remains is the three newest blocks, oldest-first: 2, 3, 4. */
    int16_t out[SAMPLES];
    int ok = i2s_capture_consume(&c, out) && block_is(out, 2);
    ok = ok && i2s_capture_consume(&c, out) && block_is(out, 3);
    ok = ok && i2s_capture_consume(&c, out) && block_is(out, 4);
    CHECK(ok, "the three newest blocks survive, oldest-first");
    CHECK(i2s_capture_available(&c) == 0, "ring drained");
}

/* ---- underrun: empty consume yields silence ---- */
static void test_underrun(void)
{
    printf("- underrun: consuming an empty ring yields silence\n");
    int16_t store[NBLOCKS * SAMPLES];
    i2s_capture_t c;
    i2s_capture_init(&c, store, NBLOCKS, FRAMES);

    int16_t out[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++) out[i] = 0x7777;
    int r = i2s_capture_consume(&c, out);
    int silent = 1;
    for (uint32_t i = 0; i < SAMPLES; i++) if (out[i] != 0) silent = 0;
    CHECK(r == 0 && silent, "empty consume returns 0 and zero-fills");
    CHECK(c.underruns == 1 && c.consumed == 0, "underrun counted, none consumed");
}

/* ---- the RX DMA control block ---- */
static void test_rx_cb(void)
{
    printf("- dma: the RX capture control block encoding\n");
    dma_cb_t cb;
    uint32_t fifo = dma_bus_periph(0xFE203004u);   /* PCM_FIFO_A */
    uint32_t mem  = dma_bus_mem(0x40010000u);
    dma_audio_rx_cb_init(&cb, fifo, mem, 512, 0xABCD0000u);

    CHECK((cb.ti & DMA_TI_SRC_DREQ) && (cb.ti & DMA_TI_DEST_INC),
          "RX: source paced by DREQ, destination increments");
    CHECK(!(cb.ti & DMA_TI_SRC_INC) && !(cb.ti & DMA_TI_DEST_DREQ),
          "RX: not the TX direction");
    CHECK(((cb.ti >> DMA_TI_PERMAP_SHIFT) & 0x1F) == DMA_DREQ_PCM_RX,
          "RX: PERMAP selects the PCM RX DREQ");
    CHECK((cb.ti & DMA_TI_INTEN) && (cb.ti & DMA_TI_WAIT_RESP),
          "RX: completion interrupt + wait-for-response set");
    CHECK(cb.source_ad == fifo && cb.dest_ad == mem &&
          cb.txfr_len == 512 && cb.nextconbk == 0xABCD0000u,
          "RX: addresses, length, and chain wired");
}

int main(void)
{
    printf("=== I2S capture ring host tests (issue #83) ===\n");
    test_fifo_wrap();
    test_overrun();
    test_underrun();
    test_rx_cb();

    if (g_fail) {
        printf("I2S-CAPTURE TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("I2S-CAPTURE TESTS: ALL PASS\n");
    return 0;
}
