/* include/midi_uart.h - DIN-5 MIDI input over BCM2711 UART3 (Issue #31, M7)
 *
 * Option B of the MIDI driver: a 5-pin DIN MIDI IN jack wired to UART3 RX
 * through an opto-isolator, read at the MIDI rate of 31250 baud, 8N1.  UART3 is
 * a PL011, so this is the PL011 bring-up at the MIDI baud plus a byte read; the
 * received bytes are fed to the parser (arch/arm64/midi.c) and the resulting
 * events pushed to a lock-free ring for the host.
 */

#ifndef MIDI_UART_H
#define MIDI_UART_H

#include "midi.h"
#include <stdint.h>

/* UART3 base on the BCM2711 (overridable for tests / other boards). */
#ifndef MIDI_UART_BASE
#define MIDI_UART_BASE 0xFE201600UL
#endif

/* PL011 reference clock (Hz); 48 MHz on the BCM2711. */
#ifndef MIDI_UART_CLK
#define MIDI_UART_CLK 48000000UL
#endif

#define MIDI_BAUD 31250u

/* Configure UART3 for 31250 baud, 8N1, receive enabled. */
void midi_uart_init(void);

/* True when a byte is waiting in the receive FIFO. */
int midi_uart_rx_ready(void);

/* Read one received byte (call only when rx_ready()). */
uint8_t midi_uart_read(void);

/* Drain the receive FIFO: read every available byte, run it through `p`, and
 * push each completed event into `ring`.  Returns the number of events pushed
 * (dropped events, if the ring is full, are not counted). */
uint32_t midi_uart_poll(midi_ring_t *ring, midi_parser_t *p);

#endif /* MIDI_UART_H */
