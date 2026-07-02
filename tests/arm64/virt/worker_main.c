/* tests/arm64/virt/worker_main.c - per-core audio workers on QEMU 'virt'
 * (Issue #74).
 *
 * Brings up all four cores and proves the M11 worker contract end to end:
 *
 *   CPU0 - the audio core.  On each 1 kHz timer IRQ (the DMA stand-in) it
 *          kicks the three workers for the new block, then refills its DMA
 *          buffer from the lock-free ring.  The kick path never blocks, so
 *          the watchdog must record zero overruns no matter what the workers
 *          do.
 *   CPU1 - a worker whose node is the audio producer: each block it writes
 *          one block of samples into the ring the DAC drains.
 *   CPU2 - a worker with two counter nodes (several nodes per worker).
 *   CPU3 - a worker whose node deliberately stalls forever at block
 *          STALL_AT, modelling a hung plugin.  Its kicks are skipped and
 *          charged to it and its node; audio must keep flowing regardless.
 *
 * The test passes when every worker came online, CPU0 serviced every callback
 * with zero watchdog overruns and zero ring underruns, the healthy workers
 * ran (essentially) every block, the stalled worker's missed blocks were
 * charged to it and its node, and after the stall is released every worker
 * drains to the exact invariant blocks + overruns == kicks.
 *
 * Lateness tolerance: QEMU's multi-threaded TCG can deschedule a vCPU for
 * milliseconds (see docs/latency.md), so a healthy worker is allowed a small
 * number of skipped blocks (TOL); the ring is primed deep enough that those
 * cannot also starve the DAC.  On hardware the tolerance would be zero.
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "audio_worker.h"
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
#define RING_CAP     8192u        /* samples; 63 blocks of headroom          */
#define FRAMES       64u          /* frames per block                        */
#define BLOCK_HZ     1000u        /* block rate (DMA IRQ stand-in)           */
#define CALLBACKS    1500u        /* run ~1.5 s                              */
#define STALL_AT     1000u        /* block at which CPU3's node hangs        */
#define PRIME_BLOCKS 32u          /* ring priming: absorbs TOL skips         */
#define TOL          (CALLBACKS / 50u)   /* 2% QEMU-scheduling slack         */

/* ---- shared state ---- */
static int16_t        g_ring_buf[RING_CAP];
static spsc_ring_t    g_ring;
static int16_t        g_dma[FRAMES * 2u];
static audio_core_t   g_ac;
static audio_worker_t g_w[3];              /* workers for CPU1..CPU3 */

static volatile uint64_t g_underruns;
static volatile uint64_t g_prod;           /* samples produced by CPU1 node */
static uint64_t          g_cnt2a, g_cnt2b; /* CPU2's two counter nodes      */
static uint64_t          g_cnt3;           /* CPU3's counter node           */
static volatile uint32_t g_stall_release;  /* lets the hung node finish     */
static uint64_t          g_seq;            /* block sequence, CPU0 only     */

/* Per-core stacks for the secondaries (16 KiB each). */
static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

/* ---- worker nodes ---- */

/* CPU1: the producer.  One block of counted samples into the DAC ring. */
static void node_produce(void *ctx)
{
    (void)ctx;
    int16_t chunk[FRAMES * 2u];
    for (uint32_t i = 0; i < FRAMES * 2u; i++)
        chunk[i] = (int16_t)((g_prod + i) & 0x7FFF);
    g_prod += spsc_write(&g_ring, chunk, FRAMES * 2u);
}

/* CPU2: plain per-block work. */
static void node_count(void *ctx)
{
    (*(uint64_t *)ctx)++;
}

/* CPU3: works, then hangs forever at block STALL_AT - a stuck plugin. */
static void node_stall(void *ctx)
{
    (void)ctx;
    g_cnt3++;
    if (g_cnt3 == STALL_AT)
        while (!__atomic_load_n(&g_stall_release, __ATOMIC_ACQUIRE))
            ;
}

/* ---- CPU0: the audio callback (timer IRQ context) ---- */
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= CALLBACKS)       /* freeze counters once done */
        return;

    g_seq++;
    for (int i = 0; i < 3; i++)           /* never blocks, never spins */
        aw_kick(&g_w[i], g_seq);

    uint64_t t0  = rd_cntpct();
    uint32_t got = audio_core_fill(&g_ac);
    uint64_t service = rd_cntpct() - t0;
    if (got < FRAMES * 2u)
        g_underruns++;
    audio_wd_account(&g_ac.wd, service);
    g_ac.serviced++;
}

/* ---- secondary-core body: just the worker loop ---- */
static void worker_entry(void *arg)
{
    aw_worker_loop((audio_worker_t *)arg);
}

static int wait_drained(audio_worker_t *w, uint64_t timeout)
{
    uint64_t start = rd_cntpct();
    while (!aw_drained(w))
        if (rd_cntpct() - start > timeout)
            return -1;
    return 0;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt per-core audio workers (issue #74) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / BLOCK_HZ;

    spsc_init(&g_ring, g_ring_buf, RING_CAP);
    audio_core_init(&g_ac, &g_ring, g_dma, FRAMES, interval / 2u);

    /* Assign nodes before the workers start; they apply from the first kick. */
    aw_init(&g_w[0], 1);
    aw_assign(&g_w[0], node_produce, 0);
    aw_init(&g_w[1], 2);
    aw_assign(&g_w[1], node_count, &g_cnt2a);
    aw_assign(&g_w[1], node_count, &g_cnt2b);
    aw_init(&g_w[2], 3);
    aw_assign(&g_w[2], node_stall, 0);

    /* Prime the ring so a worker skip cannot double as a DAC underrun. */
    int16_t zero[FRAMES * 2u];
    for (uint32_t i = 0; i < FRAMES * 2u; i++)
        zero[i] = 0;
    for (uint32_t b = 0; b < PRIME_BLOCKS; b++)
        spsc_write(&g_ring, zero, FRAMES * 2u);

    int e1 = smp_start_core(1, worker_entry, &g_w[0],
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    int e2 = smp_start_core(2, worker_entry, &g_w[1],
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e3 = smp_start_core(3, worker_entry, &g_w[2],
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    uart_printf("PSCI CPU_ON: cpu1=%d cpu2=%d cpu3=%d (0=ok)\r\n", e1, e2, e3);

    /* Wait (up to 1 s) for the three worker loops to come online. */
    uint64_t t = rd_cntpct();
    while ((!g_w[0].online || !g_w[1].online || !g_w[2].online) &&
           rd_cntpct() - t < freq)
        ;
    uint32_t online = g_w[0].online + g_w[1].online + g_w[2].online;
    uart_printf("workers online: %u/3\r\n", (unsigned)online);

    /* Run the audio core. */
    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    while (g_ac.serviced < CALLBACKS)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");

    /* Unstick CPU3's hung node, let every worker drain, stop the loops. */
    __atomic_store_n(&g_stall_release, 1u, __ATOMIC_RELEASE);
    int drained = 0;
    for (int i = 0; i < 3; i++)
        drained += (wait_drained(&g_w[i], freq) == 0);
    for (int i = 0; i < 3; i++)
        aw_stop(&g_w[i]);

    uart_printf("audio: serviced=%u underruns=%u overruns=%u worst=%u cyc (budget=%u)\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns, (unsigned)g_ac.wd.worst,
                (unsigned)g_ac.wd.budget);
    for (int i = 0; i < 3; i++)
        uart_printf("worker cpu%d: kicks=%u blocks=%u overruns=%u\r\n",
                    (int)g_w[i].cpu_id, (unsigned)g_w[i].kicks,
                    (unsigned)g_w[i].blocks, (unsigned)g_w[i].overruns);
    uart_printf("nodes: produced=%u cnt2a=%u cnt2b=%u cnt3=%u stall-node-overruns=%u\r\n",
                (unsigned)g_prod, (unsigned)g_cnt2a, (unsigned)g_cnt2b,
                (unsigned)g_cnt3, (unsigned)g_w[2].nodes[0].overruns);

    /* ---- checks ---- */
    int all_online  = (online == 3) && (e1 == 0) && (e2 == 0) && (e3 == 0);
    int cpu0_clean  = (g_ac.serviced == CALLBACKS) &&
                      (g_ac.wd.overruns == 0) && (g_underruns == 0);

    /* Healthy workers: every kick attempted, essentially every block ran. */
    int w1_ok = (g_w[0].kicks == CALLBACKS) &&
                (g_w[0].blocks + g_w[0].overruns == g_w[0].kicks) &&
                (g_w[0].overruns <= TOL) &&
                (g_prod == g_w[0].blocks * FRAMES * 2u);
    int w2_ok = (g_w[1].kicks == CALLBACKS) &&
                (g_w[1].blocks + g_w[1].overruns == g_w[1].kicks) &&
                (g_w[1].overruns <= TOL) &&
                (g_cnt2a == g_w[1].blocks) && (g_cnt2b == g_w[1].blocks);

    /* Stalled worker: charged for (roughly) every block after the hang, with
     * the overruns attributed to its node, and drained after release. */
    int w3_ok = (g_w[2].kicks == CALLBACKS) &&
                (g_w[2].blocks + g_w[2].overruns == g_w[2].kicks) &&
                (g_w[2].overruns + TOL >= CALLBACKS - STALL_AT) &&
                (g_w[2].blocks <= STALL_AT + TOL) &&
                (g_w[2].nodes[0].overruns == g_w[2].overruns);

    uart_printf("checks: online=%d cpu0-clean=%d w1=%d w2=%d w3=%d drained=%d/3\r\n",
                all_online, cpu0_clean, w1_ok, w2_ok, w3_ok, drained);

    int ok = all_online && cpu0_clean && w1_ok && w2_ok && w3_ok &&
             (drained == 3);
    uart_puts(ok ? "WORKER: PASS\r\n" : "WORKER: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
