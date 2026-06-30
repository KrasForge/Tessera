/* drivers/dma.c - BCM2711 DMA controller (Issue #17, M3)
 *
 * Legacy DMA channels live at 0xFE007000, 0x100 apart.  For audio we use a
 * single channel running a ring of two control blocks that alternately feed
 * the I2S TX FIFO, paced by the PCM TX DREQ.
 */

#include "dma.h"
#include <stdint.h>

/* ---- pure helper (host-testable) ------------------------------------ */

void dma_audio_cb_init(dma_cb_t *cb, uint32_t src_bus, uint32_t dst_bus,
                       uint32_t len_bytes, uint32_t next_bus)
{
    cb->ti = DMA_TI_SRC_INC | DMA_TI_DEST_DREQ | DMA_TI_WAIT_RESP |
             DMA_TI_INTEN | (DMA_DREQ_PCM_TX << DMA_TI_PERMAP_SHIFT);
    cb->source_ad   = src_bus;
    cb->dest_ad     = dst_bus;
    cb->txfr_len    = len_bytes;
    cb->stride      = 0;
    cb->nextconbk   = next_bus;
    cb->reserved[0] = 0;
    cb->reserved[1] = 0;
}

/* ---- MMIO driver ---------------------------------------------------- */
#ifndef HOSTTEST

#define DMA_BASE      0xFE007000UL
#define DMA_CH(n)     (DMA_BASE + (uint64_t)(n) * 0x100u)

#define DMA_CS(n)        (*(volatile uint32_t *)(DMA_CH(n) + 0x00))
#define DMA_CONBLK_AD(n) (*(volatile uint32_t *)(DMA_CH(n) + 0x04))

/* CS bits */
#define DMA_CS_ACTIVE   (1u << 0)
#define DMA_CS_END      (1u << 1)
#define DMA_CS_INT      (1u << 2)
#define DMA_CS_RESET    (1u << 31)

static void dma_delay(volatile int n) { while (n-- > 0) { } }

void dma_init_channel(int channel)
{
    DMA_CS(channel) = DMA_CS_RESET;
    dma_delay(1000);
    /* Clear any latched END/INT status. */
    DMA_CS(channel) = DMA_CS_END | DMA_CS_INT;
}

void dma_start(int channel, uint32_t first_cb_bus)
{
    DMA_CONBLK_AD(channel) = first_cb_bus;
    DMA_CS(channel) = DMA_CS_END | DMA_CS_INT;   /* clear status   */
    DMA_CS(channel) = DMA_CS_ACTIVE;             /* go             */
}

void dma_stop(int channel)
{
    DMA_CS(channel) = DMA_CS_RESET;
}

uint32_t dma_current_cb(int channel)
{
    return DMA_CONBLK_AD(channel);
}

#endif /* !HOSTTEST */
