/* tests/arm64/virt/gsched_main.c - topology-aware graph partitioning on QEMU
 * 'virt' (Issue #75).
 *
 * The M11 acceptance run: a synth -> filter -> DAC chain, driven end to end
 * through graph_control + graph_sched + the issue #74 workers, across three
 * live reconfigurations:
 *
 *   phase A (blocks 1..400)    - both plugins scheduled on one worker (CPU1):
 *                                the single-core reference output.
 *   rewire (at block ~400)     - the synth->filter edge is disconnected and
 *                                reconnected (new ring) at runtime, restaged
 *                                by the graph_control on-change hook.
 *   phase B (blocks ~800..end) - scheduling width widens to two: the planner
 *                                splits the chain across CPU1/CPU2, and the
 *                                now cross-core edge is reset + primed, adding
 *                                exactly one block of pipeline latency.
 *
 * The synth emits a deterministic per-block pattern and the filter applies a
 * deterministic transform, so the DAC stream is a pure function of the synth
 * block counter.  A checker walks the first sample of every serviced block
 * and requires it to follow that function consecutively - across both the
 * rewire and the split - up to a small bounded number of anomaly blocks
 * (the documented one-silence-block pipeline prime at the split, a couple of
 * discarded blocks at the rewire, and QEMU's TCG scheduling hiccups; see
 * docs/graph-scheduling.md).  Bit-identical steady state, shifted by the
 * pipeline latency, is exactly what "consecutive" verifies.
 *
 * Throughout all of it CPU0 must service every callback with zero watchdog
 * overruns, underruns stay TOL-bounded (zero in a calm run), the third
 * worker (CPU3) - never scheduled - must stay parked with zero kicks, and
 * the final stats line must show the split assignment.
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
#include "audio_graph.h"
#include "graph_control.h"
#include "graph_sched.h"
#include "gic.h"
#include "timer.h"
#include "exceptions.h"
#include "uart_pl011.h"
#include <stdint.h>

void uart_virt_init(void);
void exceptions_init(void);

static uint64_t rd_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* ---- test parameters ---- */
#define FRAMES       64u
#define SAMPLES      (FRAMES * 2u)      /* interleaved stereo per block     */
#define BLOCK_HZ     1000u
#define CALLBACKS    1500u
#define REWIRE_AT    400u               /* runtime rewire (same topology)   */
#define SPLIT_AT     800u               /* widen scheduling to two workers  */
#define TOL          (CALLBACKS / 50u)  /* QEMU TCG slack (see #74)         */
#define MAX_ANOM     (TOL + 8u)         /* transitions + one per late block */

/* ---- ring pool (the gc ring_new/ring_del backend) ---- */
#define POOL_RINGS   4
#define POOL_CAP     4096u              /* 32 blocks: absorbs skip backlog  */
static int16_t     g_pool_buf[POOL_RINGS][POOL_CAP];
static spsc_ring_t g_pool[POOL_RINGS];
static int         g_pool_used[POOL_RINGS];
static int         g_pool_next;

static void *pool_ring_new(void *ctx)
{
    (void)ctx;
    for (int i = 0; i < POOL_RINGS; i++) {
        int k = (g_pool_next + i) % POOL_RINGS;   /* round-robin: a rewire  */
        if (!g_pool_used[k]) {                    /* gets a fresh pointer   */
            g_pool_used[k] = 1;
            g_pool_next = k + 1;
            spsc_init(&g_pool[k], g_pool_buf[k], POOL_CAP);
            return &g_pool[k];
        }
    }
    return (void *)0;
}

static void pool_ring_del(void *ctx, void *ring)
{
    (void)ctx;
    for (int i = 0; i < POOL_RINGS; i++)
        if (ring == &g_pool[i])
            g_pool_used[i] = 0;
}

/* ---- shared state ---- */
static graph_control_t g_gc;
static graph_sched_t   g_sched;
static audio_worker_t  g_w[GS_MAX_WORKERS];
static gs_node_fn_t    g_fns[GRAPH_MAX_NODES];
static audio_core_t    g_ac;
static int16_t         g_dma[SAMPLES];

static int g_synth_n, g_filt_n;         /* graph node indices               */

static volatile uint64_t g_underruns;
static uint64_t          g_seq;         /* block sequence, CPU0 only        */
static int16_t           g_trace[CALLBACKS + 1];

static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

/* ---- the two plugins (worker nodes) ---- */

/* Ring endpoints, refreshed by the control plane after each (re)wire and
 * read with acquire loads by the workers - a rewire must never tear. */
static spsc_ring_t *volatile g_synth_out;
static spsc_ring_t *volatile g_filt_in;
static spsc_ring_t *volatile g_filt_out;

static uint32_t g_synth_blk = 1;        /* deterministic block counter      */

static int16_t synth_sample(uint32_t k, uint32_t i)
{
    return (int16_t)((k * 331u + i * 3u) & 0x7FFFu);
}

static int16_t filt_sample(int16_t x)
{
    return (int16_t)(((int32_t)x >> 1) + 1000);
}

static void node_synth(void *ctx)
{
    (void)ctx;
    spsc_ring_t *out = __atomic_load_n(&g_synth_out, __ATOMIC_ACQUIRE);
    if (!out)
        return;
    int16_t chunk[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++)
        chunk[i] = synth_sample(g_synth_blk, i);
    g_synth_blk++;
    spsc_write(out, chunk, SAMPLES);
}

static void node_filter(void *ctx)
{
    (void)ctx;
    spsc_ring_t *in  = __atomic_load_n(&g_filt_in,  __ATOMIC_ACQUIRE);
    spsc_ring_t *out = __atomic_load_n(&g_filt_out, __ATOMIC_ACQUIRE);
    if (!in || !out)
        return;
    int16_t buf[SAMPLES];
    uint32_t got = spsc_read(in, buf, SAMPLES);
    for (uint32_t i = got; i < SAMPLES; i++)
        buf[i] = 0;                     /* starved input -> silence, never a stall */
    for (uint32_t i = 0; i < SAMPLES; i++)
        buf[i] = filt_sample(buf[i]);
    spsc_write(out, buf, SAMPLES);
}

/* Point the plugin endpoints at the rings of the current wiring. */
static void refresh_io(void)
{
    int e_sf = audio_graph_find_edge(&g_gc.graph, g_synth_n, g_filt_n);
    int e_fd = audio_graph_find_edge(&g_gc.graph, g_filt_n,
                                     g_gc.graph.dac_node);
    spsc_ring_t *sf = (e_sf >= 0) ? g_gc.graph.edges[e_sf].ring : (void *)0;
    spsc_ring_t *fd = (e_fd >= 0) ? g_gc.graph.edges[e_fd].ring : (void *)0;
    __atomic_store_n(&g_synth_out, sf, __ATOMIC_RELEASE);
    __atomic_store_n(&g_filt_in,  sf, __ATOMIC_RELEASE);
    __atomic_store_n(&g_filt_out, fd, __ATOMIC_RELEASE);
}

/* ---- scheduler plumbing ---- */

/* graph_control on-change hook: every mutation stages a fresh plan. */
static void stage_hook(void *ctx)
{
    (void)ctx;
    graph_sched_stage(&g_sched, &g_gc.graph);
}

/* Edge placement changed: reset the ring; prime cross-core pipelines with
 * one block of silence - two for the DAC edge, so a single late worker block
 * cannot cascade into an underrun on QEMU's wobbly TCG timing. */
static void edge_reset_cb(void *ring, int prime, void *ctx)
{
    (void)ctx;
    spsc_ring_t *r = ring;
    if (!r)
        return;
    r->head = r->tail = 0;              /* workers are drained: safe reset  */
    if (prime) {
        int16_t zero[SAMPLES];
        for (uint32_t i = 0; i < SAMPLES; i++)
            zero[i] = 0;
        uint32_t blocks = (r == g_ac.ring) ? 2u : 1u;
        for (uint32_t b = 0; b < blocks; b++)
            spsc_write(r, zero, SAMPLES);
    }
}

/* ---- CPU0: the audio callback (timer IRQ context) ---- */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= CALLBACKS)
        return;

    graph_sched_apply(&g_sched, g_w, g_fns, edge_reset_cb, 0);

    g_seq++;
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_kick(&g_w[k], g_seq);

    uint64_t t0  = rd_cntpct();
    uint32_t got = audio_core_fill(&g_ac);
    uint64_t service = rd_cntpct() - t0;
    if (got < SAMPLES)
        g_underruns++;
    g_trace[g_seq] = g_ac.dma_buf[0];
    audio_wd_account(&g_ac.wd, service);
    g_ac.serviced++;
}

static void worker_entry(void *arg)
{
    aw_worker_loop((audio_worker_t *)arg);
}

static void wfi_until(uint64_t serviced)
{
    while (g_ac.serviced < serviced)
        __asm__ volatile("wfi");
}

static int wait_drained_all(uint64_t timeout)
{
    int n = 0;
    for (int k = 0; k < GS_MAX_WORKERS; k++) {
        uint64_t start = rd_cntpct();
        while (!aw_drained(&g_w[k]) && rd_cntpct() - start < timeout)
            ;
        n += aw_drained(&g_w[k]) ? 1 : 0;
    }
    return n;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt graph partitioning (issue #75) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / BLOCK_HZ;

    /* Control plane: graph + scheduler, re-planned on every mutation. */
    gc_ring_ops_t ops = { pool_ring_new, pool_ring_del, 0, 0, 0 };
    gc_init(&g_gc, &ops);
    graph_sched_init(&g_sched, GS_MAX_WORKERS);
    graph_sched_set_workers(&g_sched, 1);           /* phase A: one core */
    gc_set_on_change(&g_gc, stage_hook, 0);

    g_synth_n = gc_add_plugin(&g_gc, 1);
    g_filt_n  = gc_add_plugin(&g_gc, 2);
    gc_add_dac(&g_gc);
    gc_connect(&g_gc, 1, 2);                        /* synth -> filter   */
    gc_connect(&g_gc, 2, 0);                        /* filter -> DAC     */
    refresh_io();

    g_fns[g_synth_n].run = node_synth;
    g_fns[g_filt_n].run  = node_filter;

    /* The DAC drains the filter->DAC edge ring. */
    audio_core_init(&g_ac, (spsc_ring_t *)g_filt_out, g_dma, FRAMES,
                    interval / 2u);

    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_init(&g_w[k], (uint32_t)(k + 1));
    int e1 = smp_start_core(1, worker_entry, &g_w[0],
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, worker_entry, &g_w[1],
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e3 = smp_start_core(3, worker_entry, &g_w[2],
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    uart_printf("PSCI CPU_ON: cpu1=%d cpu2=%d cpu3=%d (0=ok)\r\n", e1, e2, e3);

    uint64_t t = rd_cntpct();
    while ((!g_w[0].online || !g_w[1].online || !g_w[2].online) &&
           rd_cntpct() - t < freq)
        ;
    uint32_t online = g_w[0].online + g_w[1].online + g_w[2].online;
    uart_printf("workers online: %u/3\r\n", (unsigned)online);

    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    /* Phase A: single-core reference. */
    wfi_until(REWIRE_AT);

    /* Runtime rewire: same topology, new ring; the hook restages twice and
     * only the newest plan is applied. */
    gc_disconnect(&g_gc, 1, 2);
    gc_connect(&g_gc, 1, 2);
    refresh_io();
    uart_printf("rewired synth->filter at block %u\r\n", (unsigned)g_seq);

    wfi_until(SPLIT_AT);

    /* Phase B: widen to two workers - the planner splits the chain. */
    graph_sched_set_workers(&g_sched, 2);
    graph_sched_stage(&g_sched, &g_gc.graph);
    uart_printf("staged 2-worker split at block %u\r\n", (unsigned)g_seq);

    wfi_until(CALLBACKS);
    timer_stop();
    __asm__ volatile("msr daifset, #2");

    int drained = wait_drained_all(freq);
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        aw_stop(&g_w[k]);

    /* ---- report ---- */
    char line[160];
    graph_sched_format(&g_sched, line, sizeof line);
    uart_puts(line);
    uart_puts("\r\n");
    uart_printf("audio: serviced=%u underruns=%u overruns=%u worst=%u cyc (budget=%u)\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns, (unsigned)g_ac.wd.worst,
                (unsigned)g_ac.wd.budget);
    for (int k = 0; k < GS_MAX_WORKERS; k++)
        uart_printf("worker cpu%d: kicks=%u blocks=%u overruns=%u\r\n",
                    (int)g_w[k].cpu_id, (unsigned)g_w[k].kicks,
                    (unsigned)g_w[k].blocks, (unsigned)g_w[k].overruns);

    /* ---- output continuity checker ---- */
    /* The DAC stream must follow filt(synth(k)) for consecutive k across the
     * whole run.  On a mismatch, resync k forward over a small window (a
     * rewire discards the in-flight blocks of the old ring by design); every
     * non-matching block is an anomaly.  Bit-identical steady state modulo
     * the pipeline shift == long consecutive runs, few anomalies. */
    uint32_t k = 1, matched = 0, anomalies = 0;
    for (uint32_t i = 1; i <= CALLBACKS; i++) {
        int16_t want = filt_sample(synth_sample(k, 0));
        if (g_trace[i] == want) {
            matched++;
            k++;
            continue;
        }
        anomalies++;                                  /* one per skip/glitch */
        for (uint32_t j = 1; j <= 8; j++) {           /* resync after a skip */
            if (g_trace[i] == filt_sample(synth_sample(k + j, 0))) {
                k += j + 1;
                matched++;
                break;
            }
        }
    }
    uart_printf("trace: matched=%u anomalies=%u final-k=%u\r\n",
                (unsigned)matched, (unsigned)anomalies, (unsigned)k);

    /* ---- checks ---- */
    int all_online  = (online == 3) && (e1 == 0) && (e2 == 0) && (e3 == 0);
    int cpu0_clean  = (g_ac.serviced == CALLBACKS) && (g_ac.wd.overruns == 0) &&
                      (g_underruns <= TOL);
    int split_ok    = g_sched.active.valid &&
                      (g_sched.active.n_workers == 2) &&
                      (g_sched.active.core[g_synth_n] !=
                       g_sched.active.core[g_filt_n]) &&
                      (g_sched.active.cross_edges == 1);
    int w0_ok = (g_w[0].blocks + g_w[0].overruns == g_w[0].kicks) &&
                (g_w[0].blocks > 0) && (g_w[0].overruns <= TOL);
    int w1_ok = (g_w[1].blocks + g_w[1].overruns == g_w[1].kicks) &&
                (g_w[1].blocks > 0) && (g_w[1].overruns <= TOL);
    int w2_parked   = (g_w[2].kicks == 0) && (g_w[2].blocks == 0);
    int trace_ok    = (matched >= CALLBACKS - MAX_ANOM) &&
                      (anomalies <= MAX_ANOM) &&
                      (k > SPLIT_AT);                /* covered both phases */

    uart_printf("checks: online=%d cpu0=%d split=%d w0=%d w1=%d parked=%d trace=%d drained=%d/3\r\n",
                all_online, cpu0_clean, split_ok, w0_ok, w1_ok, w2_parked,
                trace_ok, drained);

    int ok = all_online && cpu0_clean && split_ok && w0_ok && w1_ok &&
             w2_parked && trace_ok && (drained == 3);
    uart_puts(ok ? "GSCHED: PASS\r\n" : "GSCHED: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
