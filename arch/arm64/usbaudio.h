/* arch/arm64/usbaudio.h - USB Audio Class support (Theme H, issue #133)
 *
 * A USB audio interface (a class-compliant DAC/ADC) streams PCM over an
 * isochronous endpoint.  Two pieces of that are pure logic, independent of the
 * USB host controller, and are what this module provides:
 *
 *   1. Format discovery.  A device describes its stream in a USB Audio Class
 *      (UAC1) Format Type I descriptor - channel count, bytes per sample, bit
 *      resolution, and the sample rates it supports.  The host parses the
 *      descriptor set to learn how to talk to it.  Descriptors come off the wire
 *      from an untrusted device, so every field is bounds-checked.
 *
 *   2. Isochronous rate framing.  USB delivers one packet per (micro)frame at a
 *      fixed frame rate, but the audio sample rate rarely divides it evenly
 *      (44100 Hz over 1000 frames/s is 44.1 samples/frame).  The endpoint must
 *      send 44 or 45 samples per frame so the long-run average is exact - the
 *      same fractional accounting the transport uses for ticks.
 *
 * Integer only - runs anywhere.  Pure, host-tested (make test-arm-usbaudio); no
 * allocation, no libc, no FP.
 */

#ifndef ARM64_USBAUDIO_H
#define ARM64_USBAUDIO_H

#include <stdint.h>

/* UAC1 descriptor constants. */
#define UAC_CS_INTERFACE     0x24u   /* bDescriptorType: class-specific iface */
#define UAC_AS_FORMAT_TYPE   0x02u   /* bDescriptorSubtype: FORMAT_TYPE       */
#define UAC_FORMAT_TYPE_I    0x01u   /* bFormatType: Type I (PCM)             */

#define USB_AUDIO_MAX_RATES  8

typedef struct {
    uint8_t  n_channels;
    uint8_t  subframe_size;    /* bytes per sample per channel (2 = 16-bit) */
    uint8_t  bit_resolution;   /* valid bits per sample                     */
    uint8_t  n_rates;
    uint32_t rates[USB_AUDIO_MAX_RATES];   /* discrete supported rates (Hz)  */
} usb_audio_format_t;

/* Scan a UAC descriptor set for the Format Type I descriptor and fill `out`.
 * Returns 0 on success, -1 if no valid format descriptor is found or the blob is
 * malformed (a zero/oversized bLength, or a descriptor that claims more sample
 * rates than it has bytes for).  Every field is bounds-checked against `len`. */
int usb_audio_parse_format(const uint8_t *desc, int len, usb_audio_format_t *out);

/* Whether `format` advertises sample rate `sr`. */
int usb_audio_supports_rate(const usb_audio_format_t *format, uint32_t sr);

/* Bytes in an isochronous packet carrying `samples` frames of this format. */
uint32_t usb_audio_packet_bytes(const usb_audio_format_t *format, uint32_t samples);

/* ---- isochronous rate framing -------------------------------------------- */

typedef struct {
    uint32_t sample_rate;   /* audio sample rate (Hz)            */
    uint32_t frame_rate;    /* USB (micro)frames per second      */
    uint32_t acc;           /* fractional-sample accumulator     */
} usb_iso_t;

/* Set up framing for `sample_rate` Hz over `frame_rate` frames/s (e.g. 1000 for
 * full-speed, 8000 for high-speed microframes). */
void     usb_iso_init(usb_iso_t *iso, uint32_t sample_rate, uint32_t frame_rate);

/* Samples to send/expect in the next isochronous frame.  Advancing exactly
 * `frame_rate` frames delivers exactly `sample_rate` samples. */
uint32_t usb_iso_next(usb_iso_t *iso);

#endif /* ARM64_USBAUDIO_H */
