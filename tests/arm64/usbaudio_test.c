/* tests/arm64/usbaudio_test.c - host unit tests for USB Audio Class support
 * (Theme H, issue #133).
 *
 * Build/run via:  make test-arm-usbaudio
 */

#include "usbaudio.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* A minimal but valid UAC1 descriptor set: an interface header we skip over,
 * then a Format Type I descriptor (stereo, 16-bit, rates 44100 and 48000). */
static const uint8_t DESC[] = {
    /* some other class-specific descriptor we should skip */
    0x07, 0x24, 0x01, 0x00, 0x01, 0x00, 0x00,
    /* Format Type I: len=14, CS_INTERFACE, FORMAT_TYPE, TYPE_I,
       nCh=2, subframe=2, bits=16, nFreq=2, then 44100 and 48000 as 24-bit LE */
    0x0e, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x02,
    0x44, 0xac, 0x00,        /* 44100 = 0x00AC44 */
    0x80, 0xbb, 0x00,        /* 48000 = 0x00BB80 */
};

static void test_parse(void)
{
    printf("- parse a UAC1 Format Type I descriptor\n");
    usb_audio_format_t f;
    CHECK(usb_audio_parse_format(DESC, sizeof DESC, &f) == 0, "format descriptor found");
    CHECK(f.n_channels == 2, "2 channels");
    CHECK(f.subframe_size == 2 && f.bit_resolution == 16, "16-bit / 2-byte subframes");
    CHECK(f.n_rates == 2, "two discrete rates");
    CHECK(f.rates[0] == 44100 && f.rates[1] == 48000, "rates 44100 and 48000 decoded");
    CHECK(usb_audio_supports_rate(&f, 48000) == 1, "48000 supported");
    CHECK(usb_audio_supports_rate(&f, 96000) == 0, "96000 not supported");
    CHECK(usb_audio_packet_bytes(&f, 44) == 44u * 2u * 2u, "44 frames stereo 16-bit = 176 bytes");
}

static void test_parse_malformed(void)
{
    printf("- malformed descriptor sets are rejected, not over-read\n");
    usb_audio_format_t f;

    /* Empty / too short. */
    CHECK(usb_audio_parse_format(DESC, 1, &f) == -1, "1-byte blob has no format");

    /* A descriptor with bLength = 0 must not loop forever / over-read. */
    uint8_t zero_len[] = { 0x00, 0x24, 0x02, 0x01 };
    CHECK(usb_audio_parse_format(zero_len, sizeof zero_len, &f) == -1, "zero bLength rejected");

    /* A format descriptor claiming more rates than its bLength allows. */
    uint8_t liar[] = { 0x0e, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10,
                       0x08,                     /* nFreq = 8 ... */
                       0x44, 0xac, 0x00, 0x80, 0xbb, 0x00 };  /* ...but only 2 present */
    CHECK(usb_audio_parse_format(liar, sizeof liar, &f) == -1,
          "over-claimed rate count rejected");

    /* A descriptor whose bLength runs past the buffer end. */
    uint8_t over[] = { 0x20, 0x24, 0x02, 0x01 };
    CHECK(usb_audio_parse_format(over, sizeof over, &f) == -1, "bLength past the end rejected");
}

static void test_iso_framing(void)
{
    printf("- isochronous framing averages to the exact sample rate\n");
    /* 44100 Hz over 1000 frames/s: each frame 44 or 45, 1000 frames = 44100. */
    usb_iso_t iso; usb_iso_init(&iso, 44100, 1000);
    uint32_t total = 0;
    int saw44 = 0, saw45 = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t n = usb_iso_next(&iso);
        total += n;
        if (n == 44) saw44 = 1;
        if (n == 45) saw45 = 1;
        if (n != 44 && n != 45) { printf("  FAIL : frame %d had %u samples\n", i, n); g_fail++; break; }
    }
    CHECK(total == 44100, "1000 frames deliver exactly 44100 samples");
    CHECK(saw44 && saw45, "frames vary between 44 and 45 samples");

    /* An integer rate (48000/1000) is a constant 48 every frame. */
    usb_iso_init(&iso, 48000, 1000);
    int constant = 1;
    for (int i = 0; i < 100; i++) if (usb_iso_next(&iso) != 48) constant = 0;
    CHECK(constant, "48000/1000 is a constant 48 samples/frame");

    /* High-speed microframes: 96000 Hz over 8000 microframes/s = 12 each. */
    usb_iso_init(&iso, 96000, 8000);
    uint32_t t2 = 0;
    for (int i = 0; i < 8000; i++) t2 += usb_iso_next(&iso);
    CHECK(t2 == 96000, "8000 microframes deliver exactly 96000 samples");
}

int main(void)
{
    printf("=== Tessera USB Audio Class tests (Theme H, #133) ===\n");
    test_parse();
    test_parse_malformed();
    test_iso_framing();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
