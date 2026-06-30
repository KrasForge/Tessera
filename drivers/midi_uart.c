/* drivers/midi_uart.c - DIN-5 MIDI input over BCM2711 UART3 (Issue #31, M7) */

#include "midi_uart.h"
#include "midi.h"
#include <stdint.h>

/* PL011 register offsets. */
#define UART_DR    0x00
#define UART_FR    0x18
#define UART_IBRD  0x24
#define UART_FBRD  0x28
#define UART_LCRH  0x2C
#define UART_CR    0x30
#define UART_IMSC  0x38

#define FR_RXFE    (1u << 4)    /* receive FIFO empty */

#define LCRH_FEN   (1u << 4)    /* enable FIFOs       */
#define LCRH_WLEN8 (3u << 5)    /* 8 data bits        */

#define CR_UARTEN  (1u << 0)
#define CR_RXE     (1u << 9)

static volatile uint32_t *reg(uint32_t off)
{
    return (volatile uint32_t *)(uintptr_t)(MIDI_UART_BASE + off);
}

void midi_uart_init(void)
{
    *reg(UART_CR)   = 0;                 /* disable while configuring */
    *reg(UART_IMSC) = 0;                 /* mask all interrupts (polled) */

    /* Baud divisor for 31250: BAUDDIV = CLK / (16 * baud).  At 48 MHz this is
     * exactly 96, so the fractional part is 0. */
    uint32_t div   = MIDI_UART_CLK / (16u * MIDI_BAUD);
    uint32_t fract = ((MIDI_UART_CLK % (16u * MIDI_BAUD)) * 64u
                      + (16u * MIDI_BAUD) / 2u) / (16u * MIDI_BAUD);
    *reg(UART_IBRD) = div;
    *reg(UART_FBRD) = fract;

    *reg(UART_LCRH) = LCRH_WLEN8 | LCRH_FEN;
    *reg(UART_CR)   = CR_UARTEN | CR_RXE;
}

int midi_uart_rx_ready(void)
{
    return (*reg(UART_FR) & FR_RXFE) == 0;
}

uint8_t midi_uart_read(void)
{
    return (uint8_t)(*reg(UART_DR) & 0xFFu);
}

uint32_t midi_uart_poll(midi_ring_t *ring, midi_parser_t *p)
{
    uint32_t pushed = 0;
    while (midi_uart_rx_ready()) {
        midi_event_t e;
        if (midi_parse_byte(p, midi_uart_read(), &e))
            if (midi_ring_push(ring, &e))
                pushed++;
    }
    return pushed;
}
