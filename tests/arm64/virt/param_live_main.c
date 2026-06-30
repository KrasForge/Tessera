/* tests/arm64/virt/param_live_main.c - live parameter modulation on QEMU
 * 'virt' (Issue #33, M7).
 *
 * Runs the real control path on ARM: a MIDI CC is mapped to the filter's cutoff
 * parameter, enqueued on the lock-free parameter queue, drained by the filter
 * at the top of each block, and applied - while a 3 kHz tone is processed.  It
 * confirms on-target that sweeping the cutoff is click-free (the swept run's
 * worst sample step stays within the fully-open filter's natural step) and that
 * the cutoff actually moved.  A libm-free digital resonator synthesises the
 * tone.
 */

#include "plugin_abi.h"
#include "param_queue.h"
#include "param_map.h"
#include "midi.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define SR  48000u
#define BLK 256u
#define CC_CUTOFF 74u
#define PARAM_CUTOFF 0u

/* 3 kHz resonator at 48 kHz: y[n] = K*y[n-1] - y[n-2], y[n] = sin(n*w). */
#define RES_K   1.847759f   /* 2*cos(w)   */
#define RES_S1  0.382683f   /* sin(w)     */
typedef struct { float y1, y2; } osc_t;
static void osc_init(osc_t *o) { o->y1 = RES_S1; o->y2 = 0.0f; }
static float osc_next(osc_t *o) { float y = RES_K * o->y1 - o->y2; o->y2 = o->y1; o->y1 = y; return y; }

static float u2f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static uint32_t f2u(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }

static unsigned char g_store[sizeof(param_queue_t) + 16 * sizeof(param_event_t)];

/* Run `blocks` blocks; if sweep, drive CC 0..127 across the run.  Reports the
 * worst sample-to-sample step and the final-block RMS (scaled x10000, int). */
static void run(param_map_t *map, int sweep, int *max_delta_x, int *rms_x)
{
    param_queue_t *q = (param_queue_t *)g_store;
    pq_init(q, 16);
    osc_t osc; osc_init(&osc);
    float il[BLK], ir[BLK], ol[BLK], orr[BLK];
    float prev = 0.0f, md = 0.0f, sumsq = 0.0f;
    int blocks = 64;

    for (int b = 0; b < blocks; b++) {
        if (sweep) {
            midi_event_t ev = { MIDI_CC, 0, CC_CUTOFF, (uint8_t)(b * 127 / (blocks - 1)),
                                INPUT_SRC_MIDI };
            uint32_t id; float v;
            if (param_map_event(map, &ev, &id, &v))
                pq_push(q, id, f2u(v));
        }
        uint32_t id, bits;
        while (pq_pop(q, &id, &bits))
            plugin_set_param(id, u2f(bits));

        for (uint32_t i = 0; i < BLK; i++) il[i] = ir[i] = 0.5f * osc_next(&osc);
        plugin_process_block(il, ir, ol, orr, BLK);

        sumsq = 0.0f;
        for (uint32_t i = 0; i < BLK; i++) {
            float d = ol[i] - prev; if (d < 0) d = -d;
            if (d > md) md = d;
            prev = ol[i];
            sumsq += ol[i] * ol[i];
        }
    }
    *max_delta_x = (int)(md * 10000.0f);
    /* sqrt via a few Newton iterations (no libm). */
    float mean = sumsq / BLK, r = mean > 0 ? mean : 0.0001f;
    for (int k = 0; k < 8; k++) r = 0.5f * (r + mean / r);
    *rms_x = (int)(r * 10000.0f);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt live parameter modulation (issue #33) ===\r\n");

    param_map_t map; param_map_init(&map);
    param_map_bind(&map, CC_CUTOFF, PARAM_CUTOFF, 200.0f, 8000.0f);

    int open_d, open_r, low_d, low_r, swept_d, swept_r;
    plugin_init(SR, BLK); plugin_set_param(PARAM_CUTOFF, 8000.0f); run(&map, 0, &open_d, &open_r);
    plugin_init(SR, BLK); plugin_set_param(PARAM_CUTOFF, 200.0f);  run(&map, 0, &low_d,  &low_r);
    plugin_init(SR, BLK);                                          run(&map, 1, &swept_d, &swept_r);

    uart_printf("open: maxd=%d rms=%d ; low: rms=%d ; swept: maxd=%d rms=%d (x10000)\r\n",
                open_d, open_r, low_r, swept_d, swept_r);

    int click_free = (swept_d <= open_d * 3 / 2 + 100);   /* within 1.5x + slack */
    int moved      = (swept_r > low_r * 2);

    uart_printf("checks: click-free=%d cutoff-moved=%d (block=%u us)\r\n",
                click_free, moved, (unsigned)(BLK * 1000000u / SR));
    uart_puts((click_free && moved) ? "PARAM-LIVE: PASS\r\n" : "PARAM-LIVE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
