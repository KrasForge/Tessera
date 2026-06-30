/* tests/arm64/virt/midi_main.c - MIDI listener on QEMU 'virt' (Issue #31, M7).
 *
 * Exercises the live-input path on ARM: a stream of MIDI bytes (here a canned
 * "keyboard" performance, since QEMU has no DIN-5 jack - on hardware these
 * bytes come from UART3 via drivers/midi_uart.c) is fed through the parser into
 * the lock-free event ring, and a test listener drains the ring and logs each
 * Note On / Note Off / Control Change over the UART.  This is the "logged over
 * UART by a test listener" check, with the parser and ring being the same code
 * the hardware driver feeds.
 */

#include "midi.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define CAP 256u
static unsigned char g_store[sizeof(midi_ring_t) + CAP * sizeof(midi_event_t)];

/* A canned performance: C, E, G played and released (running status), then a
 * sustain-pedal CC, with a clock byte interleaved to prove it is ignored. */
static const uint8_t g_stream[] = {
    0x90, 60, 100,        /* Note On  C4  */
          60, 0,          /* Note Off C4  (running status) */
          64, 100,        /* Note On  E4  */
    0xF8,                 /* clock byte (ignored) */
          64, 0,          /* Note Off E4  */
          67, 100,        /* Note On  G4  */
          67, 0,          /* Note Off G4  */
    0xB0, 64, 127,        /* CC#64 (sustain) on */
};

static const char *type_name(uint8_t t)
{
    switch (t) {
    case MIDI_NOTE_ON:  return "NOTE_ON ";
    case MIDI_NOTE_OFF: return "NOTE_OFF";
    case MIDI_CC:       return "CC      ";
    default:            return "?";
    }
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt MIDI listener (issue #31) ===\r\n");

    midi_ring_t *ring = (midi_ring_t *)g_store;
    midi_ring_init(ring, CAP);

    /* Producer: feed the byte stream through the parser into the ring. */
    midi_parser_t p;
    midi_parser_init(&p);
    for (unsigned i = 0; i < sizeof(g_stream); i++) {
        midi_event_t e;
        if (midi_parse_byte(&p, g_stream[i], &e))
            midi_ring_push(ring, &e);
    }

    /* Listener: drain the ring and log every event. */
    unsigned non = 0, noff = 0, ncc = 0, total = 0;
    midi_event_t e;
    while (midi_ring_pop(ring, &e)) {
        uart_printf("  midi: %s ch=%u data=%u,%u\r\n",
                    type_name(e.type), (unsigned)e.channel,
                    (unsigned)e.data1, (unsigned)e.data2);
        if (e.type == MIDI_NOTE_ON)  non++;
        if (e.type == MIDI_NOTE_OFF) noff++;
        if (e.type == MIDI_CC)       ncc++;
        total++;
    }

    uart_printf("totals: note_on=%u note_off=%u cc=%u total=%u\r\n",
                non, noff, ncc, total);

    int ok = (non == 3) && (noff == 3) && (ncc == 1) && (total == 7);
    uart_puts(ok ? "MIDI: PASS\r\n" : "MIDI: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
