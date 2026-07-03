/* tests/arm64/virt/roundtrip_main.c - live-in, through an effect, out: the
 * round-trip latency demo on QEMU 'virt' (Issue #85, M14).
 *
 * The M14 "done when" criterion is a measured round-trip number: live audio in,
 * through the reference low-pass plugin, out the DAC.  This harness models the
 * whole loop with the real pieces (the I2S capture ring from issue #83, the
 * audio graph's input node from issue #84, and the state-variable low-pass
 * plugin from issue #29) and measures how long a sample takes to travel from
 * the capture edge to the DAC output using CNTPCT_EL0 - the same counter basis
 * as arch/arm64/latency.c.
 *
 * The graph is the real input -> filter -> DAC, walked in toposort order.  Two
 * one-block ring delays sit outside the graph compute, exactly where they sit
 * in hardware:
 *
 *   capture ring (1 block)          the input node reads the block the ADC/DMA
 *                                   finished during the *previous* period.
 *   DAC output ring (1 block)       the DAC emits the block the graph produced
 *                                   during the *previous* period.
 *
 * so the steady-state round trip is 2 blocks.  A modelled loopback feeds each
 * DAC-output block back into the capture source (the stand-in for a physical
 * DAC-out-to-ADC-in cable), and a DC step injected at the start gives a signal
 * to trace end to end through the filter.
 *
 * Time is paced by busy-waiting on CNTPCT to a per-block deadline (no timer IRQ
 * needed), so each period lasts one real block interval and the measured
 * round trip lands at ~2 blocks.  The harness asserts the deterministic thing -
 * the per-block delay is exactly 2, matching the buffer-accounting prediction -
 * and reports min/max/mean microseconds like the callback stats in
 * docs/latency.md.
 *
 * Built MMU-on (virt_mmu.ld) with the virt GIC bases; single core.  This
 * harness never does floating point (kernel builds -mgeneral-regs-only); it
 * moves plugin pages as raw words and defers int16<->float to rt_conv.c.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_mgr.h"
#include "graph_control.h"
#include "audio_graph.h"
#include "ring_contract.h"
#include "i2s_capture.h"
#include "latency.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

/* FP-compiled conversion helpers (rt_conv.c). */
void rt_capture_to_page(void *page, const int16_t *interleaved, uint32_t frames);
void rt_page_to_capture(int16_t *interleaved, const void *page, uint32_t frames);

extern char effect_elf_start[], effect_elf_end[];

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static uint64_t rd_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* PSCI SYSTEM_OFF: clean shutdown so the serial log is flushed and QEMU exits
 * promptly (the timeout in the Makefile is only a safety cap). */
static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

/* ---- ring backend for the control plane (a real page per edge) ---- */
static void *ring_new(void *c)
{ (void)c; uintptr_t pa = phys_alloc_page_zero(); return pa ? P2V(pa) : (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

/* ---- the capture ring, fed by the modelled loopback source ---- */
#define FRAMES   RING_BLOCK              /* match the plugin page layout    */
#define SAMPLES  (FRAMES * 2u)
#define NBLOCKS  4u
static int16_t       g_cap_store[NBLOCKS * SAMPLES];
static i2s_capture_t g_cap;

/* ---- per-plugin / input I/O pages (raw words; FP happens only in plugins) ---- */
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

typedef struct { uint32_t pid; plugin_t *pl; uint32_t *out_k; uint32_t *in_k; } io_t;
static io_t g_io[GRAPH_MAX_NODES];
static int  g_n_io;
static uint32_t g_input_out[SAMPLES];    /* the input node's output page (float bits) */

static io_t *io_by_pid(uint32_t pid)
{
    for (int i = 0; i < g_n_io; i++)
        if (g_io[i].pid == pid) return &g_io[i];
    return (io_t *)0;
}

static void setup_plugin(uint32_t pid)
{
    plugin_t *pl = pm_plugin(&g_pm, pid);
    if (!pl) return;
    io_t *e = &g_io[g_n_io++];
    e->pid = pid; e->pl = pl;
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    e->out_k = (uint32_t *)P2V(out_pa);
    e->in_k  = (uint32_t *)P2V(in_pa);
    plugin_call_init(pl, RING_SR, RING_BLOCK);
}

static void setup_input(void)
{
    io_t *e = &g_io[g_n_io++];
    e->pid = GRAPH_INPUT_PID; e->pl = 0;
    e->out_k = g_input_out; e->in_k = 0;
}

static void page_clear(uint32_t *p) { for (uint32_t i = 0; i < SAMPLES; i++) p[i] = 0; }
static void page_copy(uint32_t *d, const uint32_t *s) { for (uint32_t i = 0; i < SAMPLES; i++) d[i] = s[i]; }
static int  page_has_sound(const uint32_t *p) { if (!p) return 0; for (uint32_t i = 0; i < SAMPLES; i++) if (p[i]) return 1; return 0; }

/* ---- metadata that travels alongside a captured block ---- */
#define META_SENTINEL 0xFFFFFFFFu        /* the prime block: never measured */
typedef struct { uint64_t t_prod; uint32_t period; } rt_meta_t;

/* FIFO of metadata for blocks in the capture ring (produced, not yet consumed).
 * Popped in lockstep with the input node's single consume per graph block. */
#define MFIFO_N (NBLOCKS + 2u)
static rt_meta_t g_mfifo[MFIFO_N];
static uint32_t  g_mf_head, g_mf_tail, g_mf_count;
static void mf_push(rt_meta_t m)
{ if (g_mf_count < MFIFO_N) { g_mfifo[g_mf_head] = m; g_mf_head = (g_mf_head + 1u) % MFIFO_N; g_mf_count++; } }
static int mf_pop(rt_meta_t *out)
{ if (!g_mf_count) return 0; *out = g_mfifo[g_mf_tail]; g_mf_tail = (g_mf_tail + 1u) % MFIFO_N; g_mf_count--; return 1; }

static rt_meta_t g_cur_meta;             /* metadata of the block in this graph pass */

/* ---- one graph block in toposort order (input -> filter -> DAC) ---- */
static void run_block(void)
{
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g_gc.graph, order, GRAPH_MAX_NODES);
    if (n < 0) return;

    for (int i = 0; i < n; i++) {
        int idx = order[i];
        uint32_t pid = g_gc.graph.nodes[idx].pid;

        if (g_gc.graph.nodes[idx].type == NODE_INPUT) {
            int16_t blk[SAMPLES];
            i2s_capture_consume(&g_cap, blk);        /* silence + count on empty */
            if (!mf_pop(&g_cur_meta)) {               /* keep metadata in lockstep */
                g_cur_meta.t_prod = 0; g_cur_meta.period = META_SENTINEL;
            }
            io_t *in = io_by_pid(GRAPH_INPUT_PID);
            rt_capture_to_page(in->out_k, blk, FRAMES);
            continue;
        }
        if (g_gc.graph.nodes[idx].type == NODE_DAC)
            continue;

        io_t *me = io_by_pid(pid);
        if (!me) continue;
        page_clear(me->in_k);
        for (int e = 0; e < GRAPH_MAX_EDGES; e++)
            if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                if (u) page_copy(me->in_k, u->out_k);
            }
        plugin_call_block(me->pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
    }
}

static const uint32_t *dac_input(void)
{
    int dac = g_gc.graph.dac_node;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == dac) {
            io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
            if (u) return u->out_k;
        }
    return 0;
}

/* ---- the DAC output ring (one-block delay) ---- */
static uint32_t  g_out_ring[SAMPLES];    /* float bits, delayed one period  */
static rt_meta_t g_out_meta;
static int       g_out_valid;

/* ---- round-trip statistics (cycles) ---- */
static uint64_t g_rt_min = ~0ull, g_rt_max, g_rt_sum, g_rt_sumsq;
static uint32_t g_rt_n;
static uint32_t g_delay_ok, g_delay_bad;
static void record_rt(uint64_t cycles, uint32_t delay_blocks)
{
    if (cycles < g_rt_min) g_rt_min = cycles;
    if (cycles > g_rt_max) g_rt_max = cycles;
    g_rt_sum   += cycles;
    g_rt_sumsq += cycles * cycles;
    g_rt_n++;
    if (delay_blocks == 2u) g_delay_ok++; else g_delay_bad++;
}

/* ---- the modelled capture source ---- */
#define INJECT_LEVEL 4000                /* a DC step: low-pass passes it through */
static void make_impulse(int16_t *blk)
{ for (uint32_t i = 0; i < SAMPLES; i++) blk[i] = INJECT_LEVEL; }
static void make_silence(int16_t *blk)
{ for (uint32_t i = 0; i < SAMPLES; i++) blk[i] = 0; }

#define PERIODS   64u                    /* ~64 blocks (~340 ms at 48 kHz)  */
#define INJECT_N   2u                    /* inject a DC step for the first 2 blocks */

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt round-trip latency (issue #85) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    uint64_t freq = rd_cntfrq();
    /* One block interval in counter ticks, and the buffer-accounting numbers. */
    uint64_t interval  = (freq * (uint64_t)RING_BLOCK) / (uint64_t)RING_SR;
    uint64_t block_us  = lat_cyc_to_us(interval, freq);
    uint64_t predict_us = 2u * block_us;
    uart_printf("CNTFRQ=%u Hz, block=%u frames @ %u Hz -> %u us/block, "
                "predicted round trip = 2 blocks = %u us\r\n",
                (unsigned)freq, (unsigned)RING_BLOCK, (unsigned)RING_SR,
                (unsigned)block_us, (unsigned)predict_us);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    gc_add_dac(&g_gc);
    gc_add_input(&g_gc);                         /* the capture source singleton */
    setup_input();
    pm_register_blob(&g_pm, "effect", effect_elf_start,
                     (size_t)(effect_elf_end - effect_elf_start));

    i2s_capture_init(&g_cap, g_cap_store, NBLOCKS, FRAMES);

    long fp = pm_load(&g_pm, "effect");
    setup_plugin((uint32_t)fp);
    pm_connect(&g_pm, GRAPH_INPUT_PID, (uint32_t)fp);   /* input -> filter */
    pm_connect(&g_pm, (uint32_t)fp, 0u);                /* filter -> DAC   */

    /* Prime the capture ring with one silent block so the input node never
     * underruns on the first period; its metadata is the sentinel, so it is
     * transported but never counted as a round-trip sample. */
    int16_t silent[SAMPLES];
    make_silence(silent);
    i2s_capture_produce(&g_cap, silent);
    { rt_meta_t m = { 0, META_SENTINEL }; mf_push(m); }

    int16_t loop_blk[SAMPLES];
    uint32_t dac_sound = 0;
    int first_signal_emitted = 0;

    uint64_t deadline = rd_cntpct();
    for (uint32_t p = 0; p < PERIODS; p++) {
        /* Pace to the block boundary: each period lasts one real block
         * interval, so a 2-block round trip measures ~2 blocks of wall time. */
        while (rd_cntpct() < deadline)
            ;
        deadline += interval;

        /* 1. DAC stage: emit the block the graph produced last period. */
        int loop_valid = 0;
        if (g_out_valid) {
            uint64_t t_emit = rd_cntpct();
            if (g_out_meta.period != META_SENTINEL) {
                record_rt(t_emit - g_out_meta.t_prod, p - g_out_meta.period);
                if (page_has_sound(g_out_ring) && !first_signal_emitted) {
                    first_signal_emitted = 1;
                    uart_printf("first signal out at block %u (delay=%u blocks)\r\n",
                                (unsigned)p, (unsigned)(p - g_out_meta.period));
                }
            }
            if (page_has_sound(g_out_ring)) dac_sound++;
            rt_page_to_capture(loop_blk, g_out_ring, FRAMES);   /* DAC-out -> cable */
            loop_valid = 1;
        }

        /* 2. graph stage: input node reads last period's captured block, the
         *    filter runs, the DAC-input page updates.  Delay it one period. */
        run_block();
        const uint32_t *di = dac_input();
        if (di) {
            page_copy(g_out_ring, di);
            g_out_meta  = g_cur_meta;
            g_out_valid = 1;
        }

        /* 3. capture stage: a DC step for the first INJECT_N periods, then the
         *    loopback of the DAC output (the modelled cable). */
        int16_t src[SAMPLES];
        if (p < INJECT_N)          make_impulse(src);
        else if (loop_valid)       for (uint32_t i = 0; i < SAMPLES; i++) src[i] = loop_blk[i];
        else                       make_silence(src);
        i2s_capture_produce(&g_cap, src);
        { rt_meta_t m; m.t_prod = rd_cntpct(); m.period = p; mf_push(m); }
    }

    /* ---- results ---- */
    uint64_t mean = g_rt_n ? g_rt_sum / g_rt_n : 0;
    uint64_t mean_sq = g_rt_n ? g_rt_sumsq / g_rt_n : 0;
    uint64_t var = (mean_sq > mean * mean) ? mean_sq - mean * mean : 0;
    uint64_t min_us  = lat_cyc_to_us(g_rt_n ? g_rt_min : 0, freq);
    uint64_t max_us  = lat_cyc_to_us(g_rt_max, freq);
    uint64_t mean_us = lat_cyc_to_us(mean, freq);
    uint64_t std_us  = lat_cyc_to_us(lat_isqrt(var), freq);

    uart_printf("roundtrip: min=%uus max=%uus mean=%uus stddev=%uus samples=%u\r\n",
                (unsigned)min_us, (unsigned)max_us, (unsigned)mean_us,
                (unsigned)std_us, (unsigned)g_rt_n);
    uart_printf("delay: exactly-2-blocks=%u off-by=%u  (predicted 2 blocks)\r\n",
                (unsigned)g_delay_ok, (unsigned)g_delay_bad);
    uart_printf("capture: overruns=%u underruns=%u  dac-sound-blocks=%u\r\n",
                (unsigned)g_cap.overruns, (unsigned)g_cap.underruns,
                (unsigned)dac_sound);

    /* Data path verified end to end: the injected DC step travelled input ->
     * filter -> DAC and came out non-silent. */
    int data_path = first_signal_emitted && (dac_sound >= INJECT_N);
    /* Buffer accounting: every measured block took exactly 2 blocks - a 0-block
     * error against the prediction (well within the one-block tolerance). */
    int accounting = (g_rt_n > 0) && (g_delay_bad == 0);
    /* No lost or duplicated blocks in the loop. */
    int clean = (g_cap.overruns == 0) && (g_cap.underruns == 0);
    /* The reported round trip lands within one block of the 2-block prediction. */
    int measured_ok = g_rt_n && (mean_us + block_us >= predict_us) &&
                      (mean_us <= predict_us + block_us);

    uart_printf("checks: data-path=%d accounting=%d clean=%d measured-in-1-block=%d\r\n",
                data_path, accounting, clean, measured_ok);

    int ok = data_path && accounting && clean && measured_ok;
    uart_puts(ok ? "ROUNDTRIP: PASS\r\n" : "ROUNDTRIP: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
