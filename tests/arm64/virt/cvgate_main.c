/* tests/arm64/virt/cvgate_main.c - CV/Gate listener on QEMU 'virt'
 * (Issue #32, M7).
 *
 * Exercises the CV/Gate path on ARM with simulated inputs (QEMU has no SPI ADC
 * or analog gate; on hardware the gate level comes from a GPIO pin and the
 * pitch code from the MCP3208 over SPI0, see drivers/spi.c).  A short sequence
 * of gate edges with pitch-CV samples - interleaved with MIDI notes - is pushed
 * onto the shared event ring, and a test listener logs each event with its
 * source tag, showing CV and MIDI coexist on one ring.
 */

#include "cvgate.h"
#include "midi.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define CAP 64u
static unsigned char g_store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];

/* 12-bit ADC over 0..5 V: ~819 codes/volt, C2 at 0 V. */
static const cv_cal_t CAL = { 0, 819, 36 };

static const char *src_name(uint8_t s) { return s == INPUT_SRC_CV ? "CV  " : "MIDI"; }
static const char *type_name(uint8_t t)
{
    switch (t) {
    case MIDI_NOTE_ON:  return "NOTE_ON ";
    case MIDI_NOTE_OFF: return "NOTE_OFF";
    case MIDI_CC:       return "CC      ";
    default:            return "?";
    }
}

static void midi_bytes(midi_parser_t *p, midi_ring_t *r, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        midi_event_t e;
        if (midi_parse_byte(p, b[i], &e)) midi_ring_push(r, &e);
    }
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt CV/Gate listener (issue #32) ===\r\n");

    midi_ring_t *ring = (midi_ring_t *)g_store;
    midi_ring_init(ring, CAP);

    cvgate_t cg;
    cvgate_init(&cg, &CAL, 1);
    midi_parser_t mp;
    midi_parser_init(&mp);

    /* Interleaved performance: CV gate plays 1 V, a MIDI note is struck and
     * released, then the CV gate releases. */
    cvgate_update(&cg, 1, 819, ring);                 /* CV gate rising @1V -> note 48 */
    { uint8_t on[]  = { 0x90, 72, 100 }; midi_bytes(&mp, ring, on,  3); }
    { uint8_t off[] = { 72, 0 };          midi_bytes(&mp, ring, off, 2); }  /* running status */
    cvgate_update(&cg, 0, 819, ring);                 /* CV gate falling -> note 48 off */

    unsigned cv = 0, midi = 0, total = 0;
    midi_event_t e;
    while (midi_ring_pop(ring, &e)) {
        uart_printf("  [%s] %s note=%u vel=%u\r\n",
                    src_name(e.source), type_name(e.type),
                    (unsigned)e.data1, (unsigned)e.data2);
        if (e.source == INPUT_SRC_CV) cv++; else midi++;
        total++;
    }

    uart_printf("totals: cv=%u midi=%u total=%u\r\n", cv, midi, total);
    int ok = (cv == 2) && (midi == 2) && (total == 4);
    uart_puts(ok ? "CVGATE: PASS\r\n" : "CVGATE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
