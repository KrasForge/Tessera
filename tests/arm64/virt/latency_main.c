/* tests/arm64/virt/latency_main.c - audio latency/jitter reporting on QEMU
 * 'virt' (Issue #22).
 *
 * Demonstrates the measurement-and-reporting mechanism end to end across
 * cores:
 *
 *   CPU0 - audio core.  On every 1 kHz callback it reads CNTPCT_EL0, records
 *          the inter-callback period and the IRQ-to-thread wakeup latency, and
 *          every second publishes a statistics snapshot (seqlock).  It never
 *          touches the UART during the run.
 *   CPU1 - reporter.  Watches the snapshot and renders the required line,
 *          `audio_latency: min=.. max=.. mean=.. stddev=.. overruns=..`, over
 *          UART.  Because this runs on a different core, the slow UART write
 *          cannot perturb the audio core's timeline.
 *   CPU2 - host producer, feeding the lock-free ring.
 *
 * The test proves the reporting does not corrupt the audio cadence: every
 * callback is serviced with zero ring underruns and zero watchdog overruns
 * while the reporter prints concurrently.  (The absolute 500 us jitter bound in
 * the acceptance criteria is a real-CM4 figure; QEMU's multi-threaded TCG is
 * not cycle-accurate, so it is reported here but not asserted.)
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
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

#define RING_CAP    4096u
#define FRAMES      64u
#define BLOCK_HZ    1000u
#define DUMP_EVERY  1000u        /* publish stats once per second */
#define CALLBACKS   3000u        /* run ~3 s */

static int16_t      g_ring_buf[RING_CAP];
static spsc_ring_t  g_ring;
static int16_t      g_dma[FRAMES * 2u];
static audio_core_t g_ac;
static lat_stats_t  g_lat;

/* seqlock-published snapshot read by the reporter core. */
static volatile uint32_t g_snap_seq;
static lat_summary_t     g_snap;
static volatile uint64_t g_snap_over;

static volatile uint32_t g_run;            /* reporter may start printing  */
static volatile uint32_t g_stop;           /* secondaries should halt      */
static volatile uint32_t g_reporter_done;
static volatile uint64_t g_prod;
static volatile uint64_t g_underruns;

static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));

static void publish(const lat_summary_t *s, uint64_t overruns)
{
    __atomic_store_n(&g_snap_seq, g_snap_seq + 1u, __ATOMIC_RELEASE); /* odd */
    g_snap      = *s;
    g_snap_over = overruns;
    __atomic_store_n(&g_snap_seq, g_snap_seq + 1u, __ATOMIC_RELEASE); /* even */
}

/* CPU0 audio callback (IRQ context, via the weak scheduler_tick hook). */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    uint64_t entry = rd_cntpct();

    /* IRQ-to-thread wakeup latency: timer_tick() already advanced the compare,
     * so the deadline that just elapsed is (next - interval). */
    uint64_t fired = timer_deadline() - timer_interval();
    lat_record_wakeup(&g_lat, entry > fired ? entry - fired : 0);

    lat_record(&g_lat, entry);
    if (audio_core_fill(&g_ac) < FRAMES * 2u)
        g_underruns++;
    uint64_t service = rd_cntpct() - entry;
    audio_wd_account(&g_ac.wd, service);
    g_ac.serviced++;

    if (g_ac.serviced % DUMP_EVERY == 0) {
        lat_summary_t tmp;
        lat_summary(&g_lat, &tmp);
        publish(&tmp, g_ac.wd.overruns);
    }
}

static void reporter_fn(void)
{
    while (!__atomic_load_n(&g_run, __ATOMIC_ACQUIRE))
        ;
    uint32_t last = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        uint32_t s1 = __atomic_load_n(&g_snap_seq, __ATOMIC_ACQUIRE);
        if ((s1 & 1u) || s1 == last)
            continue;
        lat_summary_t c = g_snap;
        uint64_t over   = g_snap_over;
        uint32_t s2 = __atomic_load_n(&g_snap_seq, __ATOMIC_ACQUIRE);
        if (s2 != s1)
            continue;                       /* snapshot changed mid-read */
        last = s1;
        uart_printf("audio_latency: min=%uus max=%uus mean=%uus stddev=%uus overruns=%u\r\n",
                    (unsigned)c.min_us, (unsigned)c.max_us, (unsigned)c.mean_us,
                    (unsigned)c.stddev_us, (unsigned)over);
        uart_printf("audio_wakeup: max=%uus mean=%uus\r\n",
                    (unsigned)c.wake_max_us, (unsigned)c.wake_mean_us);
    }
    __atomic_store_n(&g_reporter_done, 1u, __ATOMIC_RELEASE);
}

static void producer_fn(void)
{
    int16_t chunk[64];
    uint32_t seq = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 64; i++)
            chunk[i] = (int16_t)(seq + (uint32_t)i);
        uint32_t w = spsc_write(&g_ring, chunk, 64);
        seq += w;
        g_prod += w;
    }
}

static void secondary_fn(void *arg)
{
    uint32_t cpu = (uint32_t)(uintptr_t)arg;
    if (cpu == 1)
        reporter_fn();
    else
        producer_fn();
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt audio latency/jitter reporting (issue #22) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uart_printf("CPU0 audio core, CNTFRQ=%u Hz, reporting every %u callbacks\r\n",
                (unsigned)freq, (unsigned)DUMP_EVERY);

    spsc_init(&g_ring, g_ring_buf, RING_CAP);
    audio_core_init(&g_ac, &g_ring, g_dma, FRAMES, freq / BLOCK_HZ / 2u);
    lat_init(&g_lat, freq);

    int e1 = smp_start_core(1, secondary_fn, (void *)(uintptr_t)1,
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, secondary_fn, (void *)(uintptr_t)2,
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    uart_printf("reporter(cpu1)=%d producer(cpu2)=%d (0=ok)\r\n", e1, e2);

    /* Let the producer prime the ring. */
    uint64_t t0 = rd_cntpct();
    while (spsc_available(&g_ring) < FRAMES * 2u)
        if (rd_cntpct() - t0 > freq) break;

    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    /* From here only the reporter core writes the UART (until we stop it),
     * so the stats lines never interleave with this core's output. */
    __atomic_store_n(&g_run, 1u, __ATOMIC_RELEASE);

    while (g_ac.serviced < CALLBACKS)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    /* Stop the reporter and wait for it to finish its last line before this
     * core prints again (keeps the UART output uncorrupted). */
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    uint64_t tw = rd_cntpct();
    while (!__atomic_load_n(&g_reporter_done, __ATOMIC_ACQUIRE))
        if (rd_cntpct() - tw > freq) break;

    lat_summary_t fin;
    lat_summary(&g_lat, &fin);
    uart_printf("final: serviced=%u underruns=%u overruns=%u\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns);
    uart_printf("final audio_latency: min=%uus max=%uus mean=%uus stddev=%uus\r\n",
                (unsigned)fin.min_us, (unsigned)fin.max_us,
                (unsigned)fin.mean_us, (unsigned)fin.stddev_us);
    uart_printf("final audio_wakeup: max=%uus mean=%uus, producer=%u samples\r\n",
                (unsigned)fin.wake_max_us, (unsigned)fin.wake_mean_us,
                (unsigned)g_prod);

    int serviced_all = (g_ac.serviced >= CALLBACKS);
    int reported     = (g_snap_seq >= 2);          /* at least one snapshot */
    int stats_sane   = (fin.count > 0) && (fin.max_us >= fin.min_us) &&
                       (fin.mean_us >= fin.min_us) && (fin.mean_us <= fin.max_us);
    int no_underrun  = (g_underruns == 0);
    int no_overrun   = (g_ac.wd.overruns == 0);

    uart_printf("checks: serviced=%d reported=%d sane=%d no-underrun=%d no-overrun=%d\r\n",
                serviced_all, reported, stats_sane, no_underrun, no_overrun);

    int ok = serviced_all && reported && stats_sane && no_underrun && no_overrun;
    uart_puts(ok ? "AUDIO-LAT: PASS\r\n" : "AUDIO-LAT: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
