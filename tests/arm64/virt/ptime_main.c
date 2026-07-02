/* tests/arm64/virt/ptime_main.c - per-plugin time accounting on QEMU 'virt'
 * (Issue #77).
 *
 * The M12 attribution machinery, live on four cores:
 *
 *   CPU0 - the audio core: kicks the workers each 1 kHz block, refills the
 *          DAC ring, never prints, never blocks.
 *   CPU1 - worker 0, clock installed: a producer node (tag 1) that feeds the
 *          DAC ring, plus a calibrated light node (tag 2, ~1/16 block).
 *   CPU2 - worker 1, clock installed: a calibrated heavy node (tag 3,
 *          ~1/3 block).
 *   CPU3 - the reporter: once per second it snapshots each worker's
 *          seqlocked stats board and renders the per-plugin lines over UART,
 *          exactly like the M4 latency reporter:
 *            plugin_time: pid=3 (heavy) runs=... min=..us max=..us mean=..us overruns=...
 *
 * The test passes when the reporter printed at least two rounds covering
 * every tag; the measured means match the calibration (the spin nodes wait
 * on CNTPCT, so they cannot finish early: light >= ~55us, heavy >= ~300us,
 * heavy > light); every settled stat is consistent (min <= mean <= max);
 * and the accounting + reporting perturbed nothing: CPU0 serviced every
 * callback with zero watchdog overruns, underruns and worker lateness stay
 * within the usual QEMU TCG tolerance (see #74).
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
#include "plugin_time.h"
#include "latency.h"
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
#define RING_CAP   8192u
#define FRAMES     64u
#define SAMPLES    (FRAMES * 2u)
#define BLOCK_HZ   1000u
#define CALLBACKS  2500u                /* ~2.5 s: at least two reports     */
/* Worker-lateness/underrun slack.  Four busy vCPUs on a four-core CI host
 * oversubscribe with QEMU's own threads, so TCG stalls a worker for
 * milliseconds a few percent of the time (same allowance as the multicore
 * harness, cf. docs/latency.md).  The strict assertions - correct and
 * consistent reporting, calibrated means, CPU0 servicing every callback
 * with zero overruns - carry no slack. */
#define TOL        (CALLBACKS / 8u)

#define TAG_PROD   1u
#define TAG_LIGHT  2u
#define TAG_HEAVY  3u
#define N_TAGS     4u                   /* index by tag, slot 0 unused      */

/* ---- shared state ---- */
static int16_t        g_ring_buf[RING_CAP];
static spsc_ring_t    g_ring;
static int16_t        g_dma[SAMPLES];
static audio_core_t   g_ac;
static audio_worker_t g_w[2];
static pt_board_t     g_board[2];

static uint64_t          g_seq;
static volatile uint64_t g_underruns;
static uint64_t          g_light_ticks, g_heavy_ticks;
static uint64_t          g_prod;

/* Reporter results (written by CPU3, read by CPU0 after it stops). */
static volatile uint32_t g_rep_stop, g_rep_done, g_rep_rounds;
static volatile uint32_t g_rep_bad;             /* inconsistent stats seen  */
static volatile uint64_t g_seen_runs[N_TAGS];   /* last runs per tag        */
static volatile uint64_t g_seen_mean[N_TAGS];   /* last mean (us) per tag   */

static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

/* ---- worker nodes ---- */
static void node_produce(void *ctx)
{
    (void)ctx;
    int16_t chunk[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++)
        chunk[i] = (int16_t)((g_prod + i) & 0x7FFF);
    g_prod += spsc_write(&g_ring, chunk, SAMPLES);
}

static void node_spin(void *ctx)
{
    uint64_t ticks = *(const uint64_t *)ctx;
    uint64_t t0 = rd_cntpct();
    while (rd_cntpct() - t0 < ticks)
        ;
}

/* ---- CPU0: audio callback (timer IRQ) ---- */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= CALLBACKS)
        return;

    g_seq++;
    aw_kick(&g_w[0], g_seq);
    aw_kick(&g_w[1], g_seq);

    uint32_t got = audio_core_fill(&g_ac);
    if (got < SAMPLES)
        g_underruns++;
    audio_wd_account(&g_ac.wd, 0);      /* cadence bookkeeping only         */
    g_ac.serviced++;
}

static void worker_entry(void *arg)
{
    aw_worker_loop((audio_worker_t *)arg);
}

/* ---- CPU3: the reporter ---- */
static const char *tag_name(uint32_t tag)
{
    switch (tag) {
    case TAG_PROD:  return "prod";
    case TAG_LIGHT: return "light";
    case TAG_HEAVY: return "heavy";
    default:        return "?";
    }
}

static void reporter_entry(void *arg)
{
    uint64_t freq = (uint64_t)(uintptr_t)arg;
    uint64_t next = rd_cntpct() + freq;

    while (!__atomic_load_n(&g_rep_stop, __ATOMIC_ACQUIRE)) {
        if (rd_cntpct() < next) {
            /* Park until something happens - the workers' kick/publish SEVs
             * arrive every block, so this wakes often enough while keeping
             * the reporter core near-idle (a busy-spinning reporter steals
             * enough vCPU time under TCG to make the workers late). */
            __asm__ volatile("wfe");
            continue;
        }
        next += freq;

        for (int k = 0; k < 2; k++) {
            pt_entry_t snap[AW_MAX_NODES];
            int n = pt_snapshot(&g_board[k], snap, AW_MAX_NODES, 1000);
            if (n < 0)
                continue;               /* worker mid-publish: next round   */
            for (int i = 0; i < n; i++) {
                char line[128];
                pt_render(&snap[i], tag_name(snap[i].tag), freq, line,
                          sizeof line);
                uart_puts(line);
                uart_puts("\r\n");

                uint64_t mean = snap[i].runs ? snap[i].sum / snap[i].runs : 0;
                if (snap[i].runs &&
                    (snap[i].min > mean || mean > snap[i].max))
                    g_rep_bad++;
                if (snap[i].tag < N_TAGS) {
                    g_seen_runs[snap[i].tag] = snap[i].runs;
                    g_seen_mean[snap[i].tag] = lat_cyc_to_us(mean, freq);
                }
            }
        }
        g_rep_rounds++;
    }
    __atomic_store_n(&g_rep_done, 1u, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("wfe");
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt per-plugin time accounting (issue #77) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / BLOCK_HZ;
    g_light_ticks = interval / 16u;     /* ~62 us at 1 kHz                  */
    g_heavy_ticks = interval / 3u;      /* ~333 us                          */

    spsc_init(&g_ring, g_ring_buf, RING_CAP);
    audio_core_init(&g_ac, &g_ring, g_dma, FRAMES, interval / 2u);

    /* Workers with clocks and stats boards; nodes tagged for attribution. */
    for (int k = 0; k < 2; k++) {
        aw_init(&g_w[k], (uint32_t)(k + 1));
        pt_board_init(&g_board[k]);
        g_w[k].clock   = rd_cntpct;
        g_w[k].publish = pt_publish;
        g_w[k].pub_ctx = &g_board[k];
    }
    int s;
    s = aw_assign(&g_w[0], node_produce, 0);          g_w[0].nodes[s].tag = TAG_PROD;
    s = aw_assign(&g_w[0], node_spin, &g_light_ticks); g_w[0].nodes[s].tag = TAG_LIGHT;
    s = aw_assign(&g_w[1], node_spin, &g_heavy_ticks); g_w[1].nodes[s].tag = TAG_HEAVY;

    /* Prime the DAC ring (one-block pipeline + TCG slack). */
    int16_t zero[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++)
        zero[i] = 0;
    for (int b = 0; b < 4; b++)
        spsc_write(&g_ring, zero, SAMPLES);

    int e1 = smp_start_core(1, worker_entry, &g_w[0],
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, worker_entry, &g_w[1],
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e3 = smp_start_core(3, reporter_entry, (void *)(uintptr_t)freq,
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    uart_printf("PSCI CPU_ON: workers=%d,%d reporter=%d (0=ok)\r\n", e1, e2, e3);

    uint64_t t = rd_cntpct();
    while ((!g_w[0].online || !g_w[1].online) && rd_cntpct() - t < freq)
        ;

    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    while (g_ac.serviced < CALLBACKS)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    int drained = 0;
    for (int k = 0; k < 2; k++) {
        uint64_t start = rd_cntpct();
        while (!aw_drained(&g_w[k]) && rd_cntpct() - start < freq)
            ;
        drained += aw_drained(&g_w[k]) ? 1 : 0;
        aw_stop(&g_w[k]);
    }

    /* Give the reporter one more second, then stop it. */
    uint64_t wait = rd_cntpct();
    while (g_rep_rounds < 2 && rd_cntpct() - wait < 2 * freq)
        ;
    __atomic_store_n(&g_rep_stop, 1u, __ATOMIC_RELEASE);
    wait = rd_cntpct();
    while (!g_rep_done && rd_cntpct() - wait < freq)
        ;

    uart_printf("audio: serviced=%u underruns=%u overruns=%u\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns);
    for (int k = 0; k < 2; k++)
        uart_printf("worker cpu%d: kicks=%u blocks=%u overruns=%u\r\n",
                    (int)g_w[k].cpu_id, (unsigned)g_w[k].kicks,
                    (unsigned)g_w[k].blocks, (unsigned)g_w[k].overruns);
    uart_printf("reporter: rounds=%u bad=%u light-mean=%uus heavy-mean=%uus\r\n",
                (unsigned)g_rep_rounds, (unsigned)g_rep_bad,
                (unsigned)g_seen_mean[TAG_LIGHT],
                (unsigned)g_seen_mean[TAG_HEAVY]);

    /* ---- checks ---- */
    int all_online = (e1 == 0) && (e2 == 0) && (e3 == 0) &&
                     g_w[0].online && g_w[1].online;
    int cpu0_ok    = (g_ac.serviced == CALLBACKS) && (g_ac.wd.overruns == 0) &&
                     (g_underruns <= TOL);
    int w_ok       = (g_w[0].blocks + g_w[0].overruns == g_w[0].kicks) &&
                     (g_w[1].blocks + g_w[1].overruns == g_w[1].kicks) &&
                     (g_w[0].overruns <= TOL) && (g_w[1].overruns <= TOL);
    int reported   = (g_rep_rounds >= 2) && (g_rep_bad == 0) &&
                     (g_seen_runs[TAG_PROD] > 0) &&
                     (g_seen_runs[TAG_LIGHT] > 0) &&
                     (g_seen_runs[TAG_HEAVY] > 0);
    /* The spin nodes wait on the counter, so they cannot finish early; the
     * means must sit at or above the calibration and order correctly. */
    int calibrated = (g_seen_mean[TAG_LIGHT] >= 55u) &&
                     (g_seen_mean[TAG_HEAVY] >= 300u) &&
                     (g_seen_mean[TAG_HEAVY] > g_seen_mean[TAG_LIGHT]);

    uart_printf("checks: online=%d cpu0=%d workers=%d reported=%d calibrated=%d drained=%d/2\r\n",
                all_online, cpu0_ok, w_ok, reported, calibrated, drained);

    int ok = all_online && cpu0_ok && w_ok && reported && calibrated &&
             (drained == 2);
    uart_puts(ok ? "PTIME: PASS\r\n" : "PTIME: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
