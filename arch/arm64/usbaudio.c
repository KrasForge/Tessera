/* arch/arm64/usbaudio.c - USB Audio Class support (Theme H, issue #133).
 * See usbaudio.h. */

#include "usbaudio.h"

/* A UAC1 Format Type I descriptor (bDescriptorSubtype FORMAT_TYPE, bFormatType
 * TYPE_I) has this fixed head, then bSamFreqType*3 bytes of 24-bit rates:
 *   0 bLength   1 bDescriptorType(0x24)  2 bDescriptorSubtype(0x02)
 *   3 bFormatType(0x01)  4 bNrChannels  5 bSubframeSize  6 bBitResolution
 *   7 bSamFreqType(n)    8.. n*3 bytes (each a 24-bit little-endian Hz value) */
#define FMT_FIXED_LEN 8

static uint32_t rd24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int usb_audio_parse_format(const uint8_t *desc, int len, usb_audio_format_t *out)
{
    int pos = 0;
    /* Walk the descriptor set (each entry: bLength, bDescriptorType, ...). */
    while (pos + 2 <= len) {
        int dlen = desc[pos];
        if (dlen < 2 || pos + dlen > len)
            return -1;                       /* malformed / truncated entry */

        if (desc[pos + 1] == UAC_CS_INTERFACE && dlen >= FMT_FIXED_LEN &&
            desc[pos + 2] == UAC_AS_FORMAT_TYPE &&
            desc[pos + 3] == UAC_FORMAT_TYPE_I) {

            uint8_t n_rates = desc[pos + 7];
            /* The descriptor must actually contain n_rates * 3 rate bytes. */
            if (n_rates == 0 || n_rates > USB_AUDIO_MAX_RATES)
                return -1;
            if (dlen < FMT_FIXED_LEN + (int)n_rates * 3)
                return -1;

            out->n_channels     = desc[pos + 4];
            out->subframe_size  = desc[pos + 5];
            out->bit_resolution = desc[pos + 6];
            out->n_rates        = n_rates;
            for (int r = 0; r < n_rates; r++)
                out->rates[r] = rd24(desc + pos + FMT_FIXED_LEN + r * 3);
            return 0;
        }
        pos += dlen;
    }
    return -1;                                /* no format descriptor found */
}

int usb_audio_supports_rate(const usb_audio_format_t *format, uint32_t sr)
{
    for (int i = 0; i < format->n_rates && i < USB_AUDIO_MAX_RATES; i++)
        if (format->rates[i] == sr)
            return 1;
    return 0;
}

uint32_t usb_audio_packet_bytes(const usb_audio_format_t *format, uint32_t samples)
{
    return samples * (uint32_t)format->n_channels * (uint32_t)format->subframe_size;
}

void usb_iso_init(usb_iso_t *iso, uint32_t sample_rate, uint32_t frame_rate)
{
    iso->sample_rate = sample_rate;
    iso->frame_rate  = frame_rate ? frame_rate : 1u;
    iso->acc         = 0;
}

uint32_t usb_iso_next(usb_iso_t *iso)
{
    /* Exact fractional accounting: carry the remainder so `frame_rate` frames
     * sum to exactly `sample_rate` samples (44100/1000 -> 44,45,45,... avg 44.1). */
    iso->acc += iso->sample_rate;
    uint32_t samples = iso->acc / iso->frame_rate;
    iso->acc -= samples * iso->frame_rate;
    return samples;
}
