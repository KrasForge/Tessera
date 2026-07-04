/* tests/arm64/virt/xfade_main.c - glitch-free crossfade patch switching on
 * QEMU 'virt' (Theme A: reliability).
 *
 * A patch swap must not click.  The harness plays a steady patch A, triggers a
 * crossfade to a new patch B, and plays steady B, feeding every DAC-bound block
 * through the crossfade mixer - the same fixed-point path the audio engine
 * would use.  It asserts the deterministic, product-relevant facts:
 *
 *   - the DAC is never silent across the switch - every block carries sound;
 *   - before the fade the DAC is exactly patch A, after it exactly patch B;
 *   - through the fade the level moves monotonically A -> B and the largest
 *     block-to-block step is far below the step an abrupt cut would make (an
 *     abrupt cut is the click; the crossfade removes it);
 *   - the two gains sum to unity at every ramp step (Q15).
 *
 * FP-free: patches are constant int16 PCM levels and the mixer is fixed-point,
 * so the whole harness builds -mgeneral-regs-only like the audio path.  Built
 * with virt.ld (no MMU, single core); no plugins are needed - the crossfade is
 * pure kernel signal handling.
 */

#include "xfade.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define BLK       128u          /* int16 samples per DAC block (64 stereo frames) */
#define PRE       3u            /* steady patch-A blocks before the switch        */
#define POST      3u            /* steady patch-B blocks after the switch         */
#define A_LEVEL   12000
#define B_LEVEL   4000

static int16_t g_a[BLK];        /* patch A's block (constant level)  */
static int16_t g_b[BLK];        /* patch B's block                   */
static int16_t g_dac[BLK];      /* the DAC-bound result              */

static void fill_const(int16_t *p, int16_t v)
{
    for (uint32_t i = 0; i < BLK; i++) p[i] = v;
}
static int has_sound(const int16_t *p)
{
    for (uint32_t i = 0; i < BLK; i++) if (p[i]) return 1;
    return 0;
}
static int all_eq(const int16_t *p, int16_t v)
{
    for (uint32_t i = 0; i < BLK; i++) if (p[i] != v) return 0;
    return 1;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt glitch-free patch switching (Theme A: reliability) ===\r\n");

    fill_const(g_a, A_LEVEL);
    fill_const(g_b, B_LEVEL);

    xf_state_t xf;
    xf_init(&xf);

    uint32_t sound = 0, pre_ok = 0, post_ok = 0;
    int mono = 1, max_step = 0, prev = A_LEVEL, fade_first = 0, fade_last = 0;

    /* Steady patch A. */
    for (uint32_t b = 0; b < PRE; b++) {
        xf_block(&xf, g_a, g_b, g_dac, BLK);
        if (has_sound(g_dac)) sound++;
        if (all_eq(g_dac, A_LEVEL)) pre_ok++;
        prev = g_dac[0];
    }

    /* Trigger the crossfade and render both patches through it. */
    xf_begin(&xf);
    for (uint32_t k = 0; xf_active(&xf); k++) {
        xf_block(&xf, g_a, g_b, g_dac, BLK);
        if (has_sound(g_dac)) sound++;
        if (k == 0) fade_first = g_dac[0];
        int v = g_dac[0];
        if (v > prev) mono = 0;                 /* A > B: expect non-increasing */
        int d = prev - v; if (d < 0) d = -d;
        if (d > max_step) max_step = d;
        prev = v;
        fade_last = v;
    }

    /* Steady patch B (B is now the running patch, so it is the mixer's A). */
    for (uint32_t b = 0; b < POST; b++) {
        xf_block(&xf, g_b, g_a, g_dac, BLK);
        if (has_sound(g_dac)) sound++;
        if (all_eq(g_dac, B_LEVEL)) post_ok++;
    }

    /* Q15 unit-gain invariant across the whole ramp. */
    int gain_ok = 1;
    for (uint32_t s = 0; s <= XF_STEPS; s++)
        if (xf_gain_b(s) + ((uint32_t)XF_ONE - xf_gain_b(s)) != (uint32_t)XF_ONE) gain_ok = 0;

    uint32_t total = PRE + (XF_STEPS + 1u) + POST;
    int abrupt = A_LEVEL - B_LEVEL;             /* the step an instant cut would make */

    uart_printf("blocks=%u fade=%u  pre-A=%u/%u post-B=%u/%u switches=%u\r\n",
                (unsigned)total, (unsigned)(XF_STEPS + 1u),
                (unsigned)pre_ok, (unsigned)PRE, (unsigned)post_ok, (unsigned)POST,
                (unsigned)xf.switches);
    uart_printf("never-silent=%u/%u  fade %d->%d monotonic=%d  max-step=%d (abrupt-cut=%d)  gains-unit=%d\r\n",
                (unsigned)sound, (unsigned)total,
                fade_first, fade_last, mono, max_step, abrupt, gain_ok);

    int ok = (sound == total) &&
             (pre_ok == PRE) && (post_ok == POST) &&
             (xf.switches == 1u) && (xf.fade_blocks == XF_STEPS + 1u) &&
             (fade_first == A_LEVEL) && (fade_last == B_LEVEL) &&
             mono && (max_step < abrupt / 8) && gain_ok;

    uart_puts(ok ? "PATCH-SWITCH: PASS\r\n" : "PATCH-SWITCH: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
