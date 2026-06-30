/* drivers/i2s.c - BCM2711 PCM/I2S audio output driver (Issue #16, M3)
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711), external PCM5102 DAC.
 *
 * The PCM peripheral is configured as I2S master (the SoC drives BCLK and
 * LRCLK), 16-bit stereo.  The bit clock is sourced from PLLD (500 MHz) via
 * the PCM clock manager with a fractional divider so that
 * LRCLK == sample rate within well under 1%.
 *
 * The PCM5102 needs no MCLK when its SCK pin is tied low, so only BCLK,
 * LRCLK and DATA are wired (GPIO 18/19/21, ALT0).
 *
 * Polled output only; DMA ring-buffer streaming is issue #17.
 */

#include "i2s.h"
#include <stdint.h>
#include <stddef.h>

/* ===================================================================== *
 * Pure helpers (compiled on the host too)
 * ===================================================================== */

pcm_div_t pcm_clock_divider(uint32_t src_hz, uint32_t target_hz)
{
    pcm_div_t d = { 0, 0 };
    if (target_hz == 0)
        return d;
    d.divi = src_hz / target_hz;
    uint32_t rem = src_hz % target_hz;
    d.divf = (uint32_t)(((uint64_t)rem * 4096u) / target_hz);
    if (d.divi > 4095) d.divi = 4095;   /* DIVI is 12-bit */
    if (d.divf > 4095) d.divf = 4095;   /* DIVF is 12-bit */
    return d;
}

uint32_t pcm_actual_hz(uint32_t src_hz, pcm_div_t d)
{
    uint64_t denom = (uint64_t)d.divi * 4096u + d.divf;
    if (denom == 0)
        return 0;
    return (uint32_t)(((uint64_t)src_hz * 4096u) / denom);
}

/* 16-point full-cycle sine table, amplitude ~30000 (Q15-ish). */
static const int16_t g_sine16[16] = {
        0,  11481,  21213,  27716,  30000,  27716,  21213,  11481,
        0, -11481, -21213, -27716, -30000, -27716, -21213, -11481,
};

int16_t i2s_sine(uint32_t *phase, uint32_t freq, uint32_t rate)
{
    /* Phase increment per sample as a 32-bit fixed-point fraction of a cycle. */
    uint32_t inc = (uint32_t)(((uint64_t)freq << 32) / (rate ? rate : 1u));
    *phase += inc;

    uint32_t idx  = (*phase >> 28) & 0xF;            /* top 4 bits: 16 entries */
    uint32_t frac = (*phase >> 12) & 0xFFFF;         /* next 16 bits: interp    */
    int32_t  a = g_sine16[idx];
    int32_t  b = g_sine16[(idx + 1) & 0xF];
    return (int16_t)(a + ((b - a) * (int32_t)frac) / 65536);
}

/* ===================================================================== *
 * MMIO driver (SoC only)
 * ===================================================================== */
#ifndef HOSTTEST

#include "gpio.h"

#define PCM_BASE   0xFE203000UL
#define CM_BASE    0xFE101000UL

#define PCM_CS_A     (*(volatile uint32_t *)(PCM_BASE + 0x00))
#define PCM_FIFO_A   (*(volatile uint32_t *)(PCM_BASE + 0x04))
#define PCM_MODE_A   (*(volatile uint32_t *)(PCM_BASE + 0x08))
#define PCM_RXC_A    (*(volatile uint32_t *)(PCM_BASE + 0x0C))
#define PCM_TXC_A    (*(volatile uint32_t *)(PCM_BASE + 0x10))
#define PCM_DREQ_A   (*(volatile uint32_t *)(PCM_BASE + 0x14))
#define PCM_INTEN_A  (*(volatile uint32_t *)(PCM_BASE + 0x18))
#define PCM_INTSTC_A (*(volatile uint32_t *)(PCM_BASE + 0x1C))
#define PCM_GRAY     (*(volatile uint32_t *)(PCM_BASE + 0x20))

/* PCM clock manager (password 0x5A in bits [31:24]). */
#define CM_PCMCTL    (*(volatile uint32_t *)(CM_BASE + 0x98))
#define CM_PCMDIV    (*(volatile uint32_t *)(CM_BASE + 0x9C))
#define CM_PASSWD    0x5A000000u
#define CM_CTL_ENAB  (1u << 4)
#define CM_CTL_BUSY  (1u << 7)
#define CM_SRC_PLLD  6u            /* 500 MHz */
#define CM_MASH_1    (1u << 9)

/* CS_A bits */
#define CS_EN     (1u << 0)
#define CS_RXON   (1u << 1)
#define CS_TXON   (1u << 2)
#define CS_TXCLR  (1u << 3)
#define CS_RXCLR  (1u << 4)
#define CS_TXD    (1u << 19)       /* TX FIFO can accept data */
#define CS_SYNC   (1u << 24)

/* MODE_A bits */
#define MODE_FSLEN_SHIFT 10
#define MODE_FLEN_SHIFT  0
#define MODE_CLKM   (1u << 23)     /* 0 = PCM clock is an output (master) */
#define MODE_FSM    (1u << 21)     /* 0 = frame sync is an output         */

static uint32_t g_bits = 16;       /* sample width */

static void delay(volatile int n) { while (n-- > 0) { } }

static void pcm_clock_setup(uint32_t rate)
{
    uint32_t bclk = rate * i2s_frame_bits(g_bits);   /* e.g. 48000 * 32 */
    pcm_div_t d = pcm_clock_divider(PLLD_HZ, bclk);

    /* Stop the clock, wait (bounded) for it to settle, then program SRC +
     * divider and re-enable.  The bounds keep the kernel from hanging if the
     * clock manager never reports BUSY (e.g. under emulation). */
    int spin;
    CM_PCMCTL = CM_PASSWD | CM_SRC_PLLD;             /* clear ENAB */
    spin = 100000;
    while ((CM_PCMCTL & CM_CTL_BUSY) && --spin > 0)
        ;
    CM_PCMDIV = CM_PASSWD | (d.divi << 12) | d.divf;
    CM_PCMCTL = CM_PASSWD | CM_SRC_PLLD | CM_MASH_1;
    CM_PCMCTL = CM_PASSWD | CM_SRC_PLLD | CM_MASH_1 | CM_CTL_ENAB;
    spin = 100000;
    while (!(CM_PCMCTL & CM_CTL_BUSY) && --spin > 0)
        ;
}

void i2s_set_sample_rate(uint32_t rate)
{
    pcm_clock_setup(rate);
}

void i2s_init(uint32_t rate)
{
    /* GPIO 18 = PCM_CLK (BCLK), 19 = PCM_FS (LRCLK), 21 = PCM_DOUT, ALT0. */
    gpio_set_function(18, GPIO_FUNC_ALT0);
    gpio_set_function(19, GPIO_FUNC_ALT0);
    gpio_set_function(21, GPIO_FUNC_ALT0);

    /* Bring up the bit clock. */
    pcm_clock_setup(rate);

    /* Disable while configuring. */
    PCM_CS_A = 0;
    delay(1000);
    PCM_CS_A = CS_EN;          /* enable the PCM block */
    delay(1000);

    /* Frame: 32 bits, frame-sync (LRCLK) high for 16 bits.  Master mode:
     * CLKM and FSM cleared so BCLK and LRCLK are outputs. */
    uint32_t frame = i2s_frame_bits(g_bits);          /* 32 */
    PCM_MODE_A = ((frame - 1) << MODE_FLEN_SHIFT) |
                 ((g_bits) << MODE_FSLEN_SHIFT);

    /* Channel config: two 16-bit channels.  Width field = width - 8; channel
     * 1 starts at clock 1 (I2S), channel 2 at clock 1 + width. */
    uint32_t wid = g_bits - 8;                        /* 8 for 16-bit */
    uint32_t ch1_pos = 1;
    uint32_t ch2_pos = 1 + g_bits;
    PCM_TXC_A = (1u << 31) |                           /* CH2 enable */
                (ch2_pos << 20) | (wid << 16) |
                (1u << 15) |                           /* CH1 enable (bit 14)? */
                (1u << 14) |
                (ch1_pos << 4) | wid;
    /* (Bit 14 = CH1EN, bit 30 = CH2EN; encode both widths/positions.) */

    /* Clear the FIFOs and start transmitting. */
    PCM_CS_A = CS_EN | CS_TXCLR | CS_RXCLR;
    delay(1000);
    PCM_CS_A = CS_EN | CS_TXON | CS_SYNC;
}

void i2s_write_stereo(int16_t left, int16_t right)
{
    /* Wait (bounded) for TX FIFO space, then write both samples.  The bound
     * keeps the kernel responsive if the FIFO never drains (e.g. no DAC
     * attached, or under emulation). */
    int spin = 100000;
    while (!(PCM_CS_A & CS_TXD) && --spin > 0)
        ;
    PCM_FIFO_A = (uint32_t)(uint16_t)left;
    spin = 100000;
    while (!(PCM_CS_A & CS_TXD) && --spin > 0)
        ;
    PCM_FIFO_A = (uint32_t)(uint16_t)right;
}

void i2s_write_samples(const int16_t *buf, size_t count)
{
    for (size_t i = 0; i < count; i++)
        i2s_write_stereo(buf[2 * i], buf[2 * i + 1]);
}

void i2s_play_tone(uint32_t freq, uint32_t count)
{
    uint32_t phase = 0;
    uint32_t rate = 48000;          /* default; matches i2s_init(48000) */
    for (uint32_t i = 0; i < count; i++) {
        int16_t s = i2s_sine(&phase, freq, rate);
        i2s_write_stereo(s, s);
    }
}

#endif /* !HOSTTEST */
