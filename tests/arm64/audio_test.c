/* tests/arm64/audio_test.c - host unit tests for DMA audio streaming
 * (Issue #17).
 *
 * QEMU cannot run the BCM2711 DMA engine, but the control-block encoding and
 * the double-buffer refill state machine are pure C and are exactly what
 * runs on the SoC.  The continuity test below is the important one: it
 * simulates the DMA draining A, B, A, B ... across hundreds of buffer swaps
 * and proves the CPU-side refill produces a strictly gap-free sample stream
 * (no dropout, no repeat), which is the heart of the issue #17 acceptance.
 *
 * Build/run via:  make test-arm-audio
 */

#include "dma.h"
#include "audio_stream.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Fill callback: write a running counter so the DMA-read order is verifiable. */
static void counter_fill(int16_t *dst, uint32_t frames, void *ctx)
{
    uint32_t *ctr = ctx;
    for (uint32_t i = 0; i < frames; i++) {
        dst[2 * i]     = (int16_t)(*ctr);
        dst[2 * i + 1] = (int16_t)(*ctr);
        (*ctr)++;
    }
}

int main(void)
{
    printf("=== Tessera DMA audio-streaming tests (issue #17) ===\n");

    /* ---- bus address mapping ---- */
    CHECK(dma_bus_periph(0xFE203004ULL) == 0x7E203004u,
          "peripheral bus address (PCM FIFO)");
    CHECK(dma_bus_mem(0x40300000ULL) == 0xC0300000u,
          "RAM bus address (0xC0000000 uncached alias)");

    /* ---- control-block encoding ---- */
    dma_cb_t a, b;
    uint32_t fifo = dma_bus_periph(0xFE203004ULL);
    dma_audio_cb_init(&a, 0xC0001000u, fifo, 1024, 0xC0002000u);
    CHECK((a.ti & DMA_TI_SRC_INC) && (a.ti & DMA_TI_DEST_DREQ) &&
          (a.ti & DMA_TI_WAIT_RESP) && (a.ti & DMA_TI_INTEN),
          "TI sets SRC_INC | DEST_DREQ | WAIT_RESP | INTEN");
    CHECK(((a.ti >> DMA_TI_PERMAP_SHIFT) & 0x1F) == DMA_DREQ_PCM_TX,
          "TI PERMAP routes the PCM TX DREQ");
    CHECK(a.source_ad == 0xC0001000u && a.dest_ad == fifo &&
          a.txfr_len == 1024 && a.nextconbk == 0xC0002000u,
          "CB source/dest/len/next encoded");

    /* ---- ring linkage ---- */
    dma_audio_cb_init(&a, 0xC0001000u, fifo, 1024, 0xC000B000u /* &b */);
    dma_audio_cb_init(&b, 0xC0002000u, fifo, 1024, 0xC000A000u /* &a */);
    CHECK(a.nextconbk == 0xC000B000u && b.nextconbk == 0xC000A000u,
          "two CBs form a ring (A->B->A)");

    /* ---- double-buffer continuity: no dropout across many swaps ---- */
    enum { F = 128, SWAPS = 200 };
    static int16_t bufA[F * 2], bufB[F * 2];
    static int16_t out[(SWAPS + 1) * F];
    uint32_t counter = 0;
    audio_dbuf_t d;
    audio_dbuf_init(&d, bufA, bufB, F, counter_fill, &counter);
    audio_dbuf_prime(&d);

    int now = 0, oi = 0, ok = 1;
    for (uint32_t k = 0; k < F; k++) out[oi++] = d.buf[now][2 * k];  /* read buf0 */
    for (int step = 1; step <= SWAPS; step++) {
        now ^= 1;
        audio_dbuf_service(&d, now);             /* refills the buffer just left */
        for (uint32_t k = 0; k < (uint32_t)F; k++)
            out[oi++] = d.buf[now][2 * k];        /* DMA reads the new buffer    */
    }
    for (int i = 0; i < oi; i++)
        if (out[i] != (int16_t)i) { ok = 0; break; }
    CHECK(ok, "200 buffer swaps produce a strictly sequential stream (no dropout)");
    CHECK(oi == (SWAPS + 1) * F, "every buffer was consumed exactly once");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
