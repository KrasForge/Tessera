/* include/dma.h - BCM2711 DMA controller for audio streaming (Issue #17, M3)
 *
 * Drives a legacy DMA channel in a two-control-block ring that continuously
 * feeds the I2S TX FIFO, so the CPU never polls the FIFO: the DMA engine
 * drains one buffer while the CPU refills the other (issue #17 double
 * buffering).  Net-new ARM subsystem (no DMA exists in the IKOS heritage).
 *
 * The control-block layout, transfer-info encoding, and bus-address mapping
 * are pure and unit-tested on the host; the channel start/stop touch MMIO.
 */

#ifndef DMA_H
#define DMA_H

#include <stdint.h>

/* BCM2711 DMA control block: 32-byte aligned, read by the DMA engine via a
 * bus address.  Field order is fixed by hardware. */
typedef struct __attribute__((aligned(32))) {
    uint32_t ti;          /* transfer information           */
    uint32_t source_ad;   /* source bus address             */
    uint32_t dest_ad;     /* destination bus address        */
    uint32_t txfr_len;    /* transfer length in bytes       */
    uint32_t stride;      /* 2D stride (unused: 0)          */
    uint32_t nextconbk;   /* bus address of the next CB     */
    uint32_t reserved[2];
} dma_cb_t;

/* Transfer-information (TI) bits. */
#define DMA_TI_INTEN        (1u << 0)
#define DMA_TI_WAIT_RESP    (1u << 3)
#define DMA_TI_DEST_DREQ    (1u << 6)
#define DMA_TI_SRC_INC      (1u << 8)
#define DMA_TI_PERMAP_SHIFT 16

/* DREQ (pacing) peripheral number for the PCM/I2S TX FIFO. */
#define DMA_DREQ_PCM_TX     2u

/* ---- pure helpers (host-testable) ----------------------------------- */

/* Bus address of a RAM physical address (0xC0000000 uncached DMA alias). */
static inline uint32_t dma_bus_mem(uint64_t phys)
{
    return (uint32_t)((phys & 0x3FFFFFFFu) | 0xC0000000u);
}

/* Bus address of a peripheral physical address (0xFExxxxxx -> 0x7Exxxxxx). */
static inline uint32_t dma_bus_periph(uint64_t phys)
{
    return (uint32_t)((phys & 0x00FFFFFFu) | 0x7E000000u);
}

/* Initialise a control block that streams `len_bytes` from a RAM buffer
 * (incrementing) to a peripheral FIFO (paced by its DREQ), raising the
 * completion interrupt, then chaining to `next_bus`. */
void dma_audio_cb_init(dma_cb_t *cb, uint32_t src_bus, uint32_t dst_bus,
                       uint32_t len_bytes, uint32_t next_bus);

/* ---- MMIO driver ---------------------------------------------------- */

/* Reset/prepare a DMA channel for audio (0..14). */
void dma_init_channel(int channel);

/* Start the channel executing the ring beginning at `first_cb_bus`. */
void dma_start(int channel, uint32_t first_cb_bus);

/* Stop the channel. */
void dma_stop(int channel);

/* Bus address of the control block the channel is currently executing
 * (CONBLK_AD), used to tell which buffer is being drained. */
uint32_t dma_current_cb(int channel);

#endif /* DMA_H */
