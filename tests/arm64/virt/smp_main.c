/* tests/arm64/virt/smp_main.c - dedicated audio-core SMP test on QEMU 'virt'
 * (Issue #21).
 *
 * Brings up all four cores and proves the core-affinity guarantee:
 *
 *   CPU0 - the audio thread.  Sleeps on WFI until its 1 kHz timer IRQ (the
 *          stand-in for the DMA-buffer-empty interrupt, since QEMU does not
 *          emulate the BCM2711 DMA), wakes, refills its DMA buffer from the
 *          lock-free ring, and sleeps again.  A watchdog times every callback.
 *   CPU1 - the host producer.  Spins feeding stereo samples into the ring.
 *   CPU2, CPU3 - pure busy-loop load, to prove the audio core is unaffected by
 *          heavy work on the other cores.
 *
 * Secondary cores are started with PSCI CPU_ON.  The test passes when every
 * core came online, the load/producer cores actually ran, every audio callback
 * was serviced with no ring underrun, and the watchdog recorded zero overruns
 * (the audio cadence held despite the load on CPU1-3).
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
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

/* ---- shared state ---- */
#define RING_CAP   4096u
#define FRAMES     64u            /* frames refilled per audio callback   */
#define BLOCK_HZ   1000u          /* audio callback rate (DMA IRQ stand-in)*/
#define CALLBACKS  2000u          /* run for 2000 callbacks (~2 s)        */

static int16_t      g_ring_buf[RING_CAP];
static spsc_ring_t  g_ring;
static int16_t      g_dma[FRAMES * 2u];
static audio_core_t g_ac;

static volatile uint32_t g_started;          /* secondaries that came online */
static volatile uint32_t g_stop;             /* signal load/producer to halt */
static volatile uint64_t g_busy[MAX_CPUS];   /* iterations per load core     */
static volatile uint64_t g_prod;             /* samples produced by CPU1     */
static volatile uint64_t g_underruns;        /* audio callbacks that starved */

static uint64_t g_interval;                  /* timer ticks per callback     */
static uint64_t g_prev_entry;
static volatile uint64_t g_max_jitter;

/* Per-core stacks for the secondaries (16 KiB each). */
static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

/* Audio callback: invoked from the IRQ dispatcher (irq.c) on each timer tick
 * via the weak scheduler_tick hook.  Runs alone on CPU0 in IRQ context. */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    uint64_t entry = rd_cntpct();
    if (g_prev_entry) {
        uint64_t delta = entry - g_prev_entry;
        uint64_t jit = (delta > g_interval) ? delta - g_interval
                                            : g_interval - delta;
        if (jit > g_max_jitter)
            g_max_jitter = jit;
    }
    g_prev_entry = entry;

    uint64_t t0  = rd_cntpct();
    uint32_t got = audio_core_fill(&g_ac);
    uint64_t service = rd_cntpct() - t0;
    if (got < FRAMES * 2u)
        g_underruns++;
    audio_wd_account(&g_ac.wd, service);
    g_ac.serviced++;
}

/* Secondary-core bodies, dispatched by cpu id. */
static void secondary_fn(void *arg)
{
    uint32_t cpu = (uint32_t)(uintptr_t)arg;
    __atomic_fetch_add(&g_started, 1u, __ATOMIC_RELEASE);

    if (cpu == 1) {
        /* Host producer: keep the ring fed with a counted sample stream. */
        int16_t chunk[64];
        uint32_t seq = 0;
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
            for (int i = 0; i < 64; i++)
                chunk[i] = (int16_t)(seq + (uint32_t)i);
            uint32_t w = spsc_write(&g_ring, chunk, 64);
            seq += w;
            g_prod += w;
        }
    } else {
        /* Pure CPU load on CPU2/CPU3. */
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
            g_busy[cpu]++;
    }
}

static int wait_until(volatile uint32_t *flag, uint32_t want, uint64_t timeout)
{
    uint64_t start = rd_cntpct();
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) < want)
        if (rd_cntpct() - start > timeout)
            return -1;
    return 0;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt dedicated audio-core SMP test (issue #21) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    g_interval = freq / BLOCK_HZ;
    uart_printf("CPU0 online (audio core), CNTFRQ=%u Hz\r\n", (unsigned)freq);

    spsc_init(&g_ring, g_ring_buf, RING_CAP);
    audio_core_init(&g_ac, &g_ring, g_dma, FRAMES, g_interval / 2u);

    /* Start the three secondary cores via PSCI. */
    int e1 = smp_start_core(1, secondary_fn, (void *)(uintptr_t)1,
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, secondary_fn, (void *)(uintptr_t)2,
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e3 = smp_start_core(3, secondary_fn, (void *)(uintptr_t)3,
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    uart_printf("PSCI CPU_ON: cpu1=%d cpu2=%d cpu3=%d (0=ok)\r\n", e1, e2, e3);

    /* Wait for all three to report online. */
    int up = wait_until(&g_started, 3, freq);   /* up to 1 s */
    uart_printf("secondaries online: %u/3\r\n", (unsigned)g_started);

    /* Wait until the producer has primed the ring so the first callbacks do
     * not see a cold-start underrun. */
    uint64_t prime_start = rd_cntpct();
    while (spsc_available(&g_ring) < FRAMES * 2u)
        if (rd_cntpct() - prime_start > freq) break;

    /* Bring up the audio core's interrupt path and run the callback loop. */
    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");        /* unmask IRQ on CPU0 */

    while (g_ac.serviced < CALLBACKS)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    /* Release the secondaries. */
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev");

    uart_printf("audio: serviced=%u underruns=%u overruns=%u worst=%u cyc (budget=%u)\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns, (unsigned)g_ac.wd.worst,
                (unsigned)g_ac.wd.budget);
    uart_printf("jitter: max=%u cyc (interval=%u cyc)\r\n",
                (unsigned)g_max_jitter, (unsigned)g_interval);
    uart_printf("load: cpu1 produced=%u, cpu2 busy=%u, cpu3 busy=%u\r\n",
                (unsigned)g_prod, (unsigned)g_busy[2], (unsigned)g_busy[3]);

    int all_online   = (up == 0) && (g_started == 3);
    int load_ran     = (g_prod > 0) && (g_busy[2] > 0) && (g_busy[3] > 0);
    int serviced_all = (g_ac.serviced >= CALLBACKS);
    int no_underrun  = (g_underruns == 0);
    int no_overrun   = (g_ac.wd.overruns == 0);

    uart_printf("checks: online=%d load=%d serviced=%d no-underrun=%d no-overrun=%d\r\n",
                all_online, load_ran, serviced_all, no_underrun, no_overrun);

    int ok = all_online && load_ran && serviced_all && no_underrun && no_overrun;
    uart_puts(ok ? "AUDIO-CORE: PASS\r\n" : "AUDIO-CORE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
