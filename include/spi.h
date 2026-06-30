/* include/spi.h - BCM2711 SPI0 master + MCP3208 ADC read (Issue #32, M7)
 *
 * The CM4 has no built-in ADC, so Pitch CV is read through an external SPI ADC
 * (an MCP3208).  This is the BCM2711 SPI0 controller (0xFE204000) driving the
 * ADC; the ADC command/response decode is in arch/arm64/cvgate.c.  The base
 * address is overridable for other boards / tests.
 */

#ifndef SPI_H
#define SPI_H

#include "cvgate.h"
#include "midi.h"
#include <stdint.h>

#ifndef SPI0_BASE
#define SPI0_BASE 0xFE204000UL
#endif

/* Bring up SPI0 (GPIO 7-11 to ALT0, a conservative clock divider). */
void spi_init(void);

/* Full-duplex transfer of `len` bytes: send tx[], receive into rx[]. */
void spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

/* Read single-ended channel 0..7 of the MCP3208; returns the 12-bit code. */
uint16_t spi_mcp3208_read(uint8_t channel);

/* Hardware poll: read the gate pin and the pitch-CV ADC channel, run them
 * through `cg`, and push an event to `ring` on a gate edge.  Returns 1 if an
 * event was pushed.  (Not exercised under QEMU - there is no SPI ADC there.) */
int cvgate_poll_hw(cvgate_t *cg, midi_ring_t *ring, uint32_t gate_pin,
                   uint8_t adc_channel);

#endif /* SPI_H */
