/* tests/arm64/virt/i2s_rx_main.c - I2S capture on the QEMU 'virt' board
 * (Issue #83).
 *
 * QEMU does not emulate the BCM2711 PCM/I2S peripheral, so this harness does
 * two things:
 *
 *   1. Smoke: it maps scratch RAM at the GPIO / PCM / clock / DMA register
 *      pages and runs the real i2s_init + i2s_capture_enable +
 *      i2s_capture_dma_start against them.  If any RX register access or the
 *      DMA control-block setup faulted, the exception handler would print and
 *      halt before the PASS line.
 *
 *   2. Behaviour: a modelled capture source (standing in for the DMA
 *      completion path) feeds a known ramp pattern into the capture ring, and
 *      the harness verifies the samples come back bit-exact across several
 *      ring wrap-arounds, that a stalled consumer produces exactly the
 *      expected drop-the-oldest overruns, and that draining an empty ring
 *      underruns to silence.
 *
 * Built MMU-on with the virt GIC bases; single core.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "i2s.h"
#include "i2s_capture.h"
#include "dma.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);

#define GPIO_PAGE  0xFE200000UL
#define PCM_PAGE   0xFE203000UL
#define CM_PAGE    0xFE101000UL
#define DMA_PAGE   0xFE007000UL
#define PCM_CS_A   (*(volatile uint32_t *)(PCM_PAGE + 0x00))

/* ---- modelled capture source: a monotonic stereo ramp ---- */
#define FRAMES   64u
#define SAMPLES  (FRAMES * 2u)
#define NBLOCKS  4u

static int16_t g_ring_store[NBLOCKS * SAMPLES];
static i2s_capture_t g_cap;

/* Deterministic source sample: block `seq`, sample `i`. */
static int16_t src_sample(uint32_t seq, uint32_t i)
{
    return (int16_t)((seq * 131u + i) & 0x7FFF);
}
static void source_block(int16_t *blk, uint32_t seq)
{
    for (uint32_t i = 0; i < SAMPLES; i++)
        blk[i] = src_sample(seq, i);
}
static int block_matches(const int16_t *blk, uint32_t seq)
{
    for (uint32_t i = 0; i < SAMPLES; i++)
        if (blk[i] != src_sample(seq, i))
            return 0;
    return 1;
}

/* A cache-line-ish aligned DMA control block in RAM. */
static dma_cb_t g_rx_cb __attribute__((aligned(32)));

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt I2S capture (issue #83) ===\r\n");

    pmm_init();
    mmu_init();

    /* ---- 1. smoke: RX register + DMA setup must not fault ---- */
    vmm_map(phys_alloc_page(), GPIO_PAGE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), PCM_PAGE,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), CM_PAGE,   PAGE_SIZE, VMM_READ | VMM_WRITE);
    vmm_map(phys_alloc_page(), DMA_PAGE,  PAGE_SIZE, VMM_READ | VMM_WRITE);

    i2s_init(48000);
    i2s_capture_enable();
    uart_puts("i2s_capture_enable() returned\r\n");

    uint32_t cb_bus  = dma_bus_mem((uintptr_t)&g_rx_cb);
    uint32_t buf_bus = dma_bus_mem((uintptr_t)g_ring_store);
    i2s_capture_dma_start(5, cb_bus, &g_rx_cb, buf_bus, SAMPLES * 2u);
    int cb_ok = (g_rx_cb.ti & DMA_TI_SRC_DREQ) && (g_rx_cb.ti & DMA_TI_DEST_INC) &&
                (((g_rx_cb.ti >> DMA_TI_PERMAP_SHIFT) & 0x1F) == DMA_DREQ_PCM_RX);
    uart_printf("i2s_capture_dma_start: rx-cb-valid=%d\r\n", cb_ok);

    /* ---- 2. behaviour: modelled source into the capture ring ---- */
    i2s_capture_init(&g_cap, g_ring_store, NBLOCKS, FRAMES);

    /* Stream 40 blocks through a 4-slot ring, consuming each immediately:
     * the ring wraps ten times and every block must be bit-exact. */
    int exact = 1;
    for (uint32_t seq = 0; seq < 40; seq++) {
        int16_t in[SAMPLES], out[SAMPLES];
        source_block(in, seq);
        i2s_capture_produce(&g_cap, in);
        if (!i2s_capture_consume(&g_cap, out) || !block_matches(out, seq))
            exact = 0;
    }
    uart_printf("streamed: produced=%u consumed=%u overruns=%u bit-exact=%d\r\n",
                (unsigned)g_cap.produced, (unsigned)g_cap.consumed,
                (unsigned)g_cap.overruns, exact);

    /* Stall the consumer: produce 4 (fills the ring) + 3 more -> 3 overruns.
     * Then the survivors must be the three newest blocks, oldest-first. */
    i2s_capture_init(&g_cap, g_ring_store, NBLOCKS, FRAMES);
    for (uint32_t seq = 100; seq < 107; seq++) {
        int16_t in[SAMPLES];
        source_block(in, seq);
        i2s_capture_produce(&g_cap, in);
    }
    int drop_ok = (i2s_capture_available(&g_cap) == NBLOCKS) &&
                  (g_cap.overruns == 3);
    int16_t out[SAMPLES];
    for (uint32_t seq = 103; seq < 107; seq++)
        drop_ok = drop_ok && i2s_capture_consume(&g_cap, out) &&
                  block_matches(out, seq);
    uart_printf("stalled: overruns=%u survivors-correct=%d\r\n",
                (unsigned)g_cap.overruns, drop_ok);

    /* Underrun: the now-empty ring yields silence. */
    int r = i2s_capture_consume(&g_cap, out);
    int silent = 1;
    for (uint32_t i = 0; i < SAMPLES; i++) if (out[i] != 0) silent = 0;
    int under_ok = (r == 0) && silent && (g_cap.underruns == 1);
    uart_printf("underrun: empty-consume-silent=%d\r\n", under_ok);

    int ok = cb_ok && exact && (g_cap.overruns == 3) && drop_ok && under_ok;
    uart_printf("checks: cb=%d bit-exact=%d overrun=%d underrun=%d\r\n",
                cb_ok, exact, drop_ok, under_ok);
    uart_puts(ok ? "I2S-RX: PASS\r\n" : "I2S-RX: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
