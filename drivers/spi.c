/* drivers/spi.c - BCM2711 SPI0 master + MCP3208 ADC read (Issue #32, M7) */

#include "spi.h"
#include "cvgate.h"
#include "gpio.h"
#include <stdint.h>

/* SPI0 register offsets. */
#define SPI_CS    0x00
#define SPI_FIFO  0x04
#define SPI_CLK   0x08

/* CS register bits. */
#define CS_TA       (1u << 7)    /* transfer active            */
#define CS_CLEAR_RX (1u << 5)    /* clear RX FIFO              */
#define CS_CLEAR_TX (1u << 4)    /* clear TX FIFO              */
#define CS_DONE     (1u << 16)   /* transfer done             */
#define CS_RXD      (1u << 17)   /* RX FIFO contains data     */
#define CS_TXD      (1u << 18)   /* TX FIFO has space         */

static volatile uint32_t *reg(uint32_t off)
{
    return (volatile uint32_t *)(uintptr_t)(SPI0_BASE + off);
}

void spi_init(void)
{
    /* SPI0 on GPIO 7-11 (ALT0): CE1, CE0, MISO, MOSI, SCLK. */
    gpio_set_function(7,  GPIO_FUNC_ALT0);
    gpio_set_function(8,  GPIO_FUNC_ALT0);
    gpio_set_function(9,  GPIO_FUNC_ALT0);
    gpio_set_function(10, GPIO_FUNC_ALT0);
    gpio_set_function(11, GPIO_FUNC_ALT0);

    /* Clock divider: core clock / 256 (~1.5 MHz), comfortably within the
     * MCP3208's range; must be a power of two. */
    *reg(SPI_CLK) = 256u;
    *reg(SPI_CS)  = CS_CLEAR_RX | CS_CLEAR_TX;
}

void spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    *reg(SPI_CS) = CS_CLEAR_RX | CS_CLEAR_TX | CS_TA;   /* begin transfer */

    for (uint32_t i = 0; i < len; i++) {
        while (!(*reg(SPI_CS) & CS_TXD))
            ;
        *reg(SPI_FIFO) = tx[i];
        while (!(*reg(SPI_CS) & CS_RXD))
            ;
        rx[i] = (uint8_t)(*reg(SPI_FIFO) & 0xFFu);
    }

    while (!(*reg(SPI_CS) & CS_DONE))
        ;
    *reg(SPI_CS) = CS_CLEAR_RX | CS_CLEAR_TX;           /* deassert TA */
}

uint16_t spi_mcp3208_read(uint8_t channel)
{
    uint8_t tx[3], rx[3];
    mcp3208_command(channel, tx);
    spi_transfer(tx, rx, 3);
    return mcp3208_decode(rx);
}

int cvgate_poll_hw(cvgate_t *cg, midi_ring_t *ring, uint32_t gate_pin,
                   uint8_t adc_channel)
{
    int      gate  = gpio_get(gate_pin);
    uint16_t pitch = spi_mcp3208_read(adc_channel);
    return cvgate_update(cg, gate, pitch, ring);
}
