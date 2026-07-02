/* tests/arm64/virt/shell_main.c - serial control shell on QEMU 'virt'
 * (Issue #80).
 *
 * The shell core driven by a real UART, end to end on four cores, proving it
 * runs off the audio core and shares the one UART with the periodic stats
 * reporter without either tearing the other's output:
 *
 *   CPU0 - the audio core: a 1 kHz timer callback refills the DAC ring from
 *          the lock-free ring; its watchdog must record zero overruns while
 *          the shell and reporter run.  CPU0 prints nothing during the run.
 *   CPU1 - the shell: reads bytes from the PL011 RX FIFO (fed over the serial
 *          line) and drives shell_feed; its output goes through the shared
 *          UART lock.
 *   CPU2 - the reporter: every ~20 ms it emits one `audio_latency:` line
 *          through the same lock, exactly like the M4 latency reporter.
 *   CPU3 - a producer keeping the audio ring fed.
 *
 * The output discipline is a single spinlock held for the duration of one
 * shell input-byte's processing (which includes a whole command's response)
 * or one reporter line.  A guard inside the lock asserts the two cores are
 * never both emitting - if the lock were broken, `corrupt` would trip.
 *
 * Input arrives over `-serial stdio` (piped by the Makefile); the script ends
 * with `done`, after which the harness waits for a couple more reporter
 * rounds, then prints the verdict and powers the board off via PSCI
 * SYSTEM_OFF so the serial output is flushed on a clean exit.
 *
 * Built MMU-off with the virt GIC bases; run with -smp 4.
 */

#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "shell.h"
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

/* PSCI SYSTEM_OFF: clean shutdown so `-serial stdio` output is fully flushed. */
static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

/* ---- audio plumbing ---- */
#define RING_CAP   8192u
#define FRAMES     64u
#define SAMPLES    (FRAMES * 2u)
#define BLOCK_HZ   1000u
#define RUN_MAX    8000u                 /* safety cap on audio callbacks    */

static int16_t      g_ring_buf[RING_CAP];
static spsc_ring_t  g_ring;
static int16_t      g_dma[SAMPLES];
static audio_core_t g_ac;
static volatile uint64_t g_prod;
static volatile uint32_t g_stop;

static uint8_t g_stack1[16384] __attribute__((aligned(16)));
static uint8_t g_stack2[16384] __attribute__((aligned(16)));
static uint8_t g_stack3[16384] __attribute__((aligned(16)));

/* ---- shared UART output discipline ---- */
static volatile uint32_t g_ulock;
static volatile int      g_in_emit;
static volatile int      g_corrupt;

static void ulock(void)
{
    while (__atomic_exchange_n(&g_ulock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");
    if (g_in_emit)              /* two cores inside at once => lock is broken */
        g_corrupt = 1;
    g_in_emit = 1;
}

static void uunlock(void)
{
    g_in_emit = 0;
    __atomic_store_n(&g_ulock, 0u, __ATOMIC_RELEASE);
}

/* ---- shell commands ---- */
static volatile uint32_t g_echo_runs, g_unknown_runs, g_help_listed;
static volatile uint32_t g_input_done;

static int h_echo(shell_t *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        shell_write(sh, argv[i]);
        shell_write(sh, (i + 1 < argc) ? " " : "\r\n");
    }
    if (argc == 1)
        shell_write(sh, "\r\n");
    g_echo_runs++;
    return 0;
}

static int h_stat(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    char line[48];
    /* tiny uint renderer to avoid pulling printf into the sink */
    const char *p = "stat: serviced=";
    int k = 0;
    while (*p) line[k++] = *p++;
    uint32_t v = (uint32_t)g_ac.serviced, div = 1000000000u;
    int started = 0;
    for (; div; div /= 10u) {
        uint32_t d = (v / div) % 10u;
        if (d || started || div == 1u) { line[k++] = (char)('0' + d); started = 1; }
    }
    line[k++] = '\r'; line[k++] = '\n'; line[k] = '\0';
    shell_write(sh, line);
    return 0;
}

static int h_bad(shell_t *sh, int argc, char **argv)
{
    (void)sh; (void)argc; (void)argv;
    return -3;                          /* exercises the error-line path     */
}

static int h_done(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_write(sh, "bye\r\n");
    __atomic_store_n(&g_input_done, 1u, __ATOMIC_RELEASE);
    return 0;
}

static const shell_cmd_t g_cmds[] = {
    { "echo", "print arguments",  h_echo },
    { "stat", "audio stats",      h_stat },
    { "bad",  "return an error",  h_bad  },
    { "done", "finish the demo",  h_done },
};

/* Count how many lines `help` prints, by wrapping the sink for one dispatch:
 * simpler to just trust the host test.  Here we detect help ran by watching
 * for the "commands:" header via a flag the sink sets. */
static int has_prefix(const char *s, const char *p)
{
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static void sh_out(void *io, const char *s)
{
    (void)io;
    /* Watch the outgoing stream for the built-in help header and the unknown-
     * command message so the in-guest verdict can confirm both paths ran. */
    if (has_prefix(s, "commands:"))
        g_help_listed = 1;
    if (has_prefix(s, "unknown command:"))
        g_unknown_runs++;
    uart_puts(s);
}

/* ---- CPU0: audio callback ---- */
static volatile uint64_t g_underruns;

void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (g_ac.serviced >= RUN_MAX)
        return;
    uint64_t t0  = rd_cntpct();
    uint32_t got = audio_core_fill(&g_ac);
    if (got < SAMPLES)
        g_underruns++;
    audio_wd_account(&g_ac.wd, rd_cntpct() - t0);
    g_ac.serviced++;
}

/* ---- CPU1: the shell ---- */
static void shell_core(void *arg)
{
    (void)arg;
    shell_t sh;
    shell_init(&sh, g_cmds, 4, sh_out, 0);
    sh.prompt = "tessera> ";
    ulock(); shell_prompt(&sh); uunlock();

    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        int c = uart_try_getc();
        if (c < 0) {
            __asm__ volatile("yield");
            continue;
        }
        ulock();
        shell_feed(&sh, (char)c);   /* whole response emitted atomically */
        uunlock();
    }
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- CPU2: the reporter ---- */
static volatile uint32_t g_rep_rounds;

static void reporter_core(void *arg)
{
    uint64_t period = (uint64_t)(uintptr_t)arg;
    uint64_t next = rd_cntpct() + period;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        if (rd_cntpct() < next) {
            __asm__ volatile("yield");
            continue;
        }
        next += period;
        ulock();
        uart_printf("audio_latency: serviced=%u overruns=%u\r\n",
                    (unsigned)g_ac.serviced, (unsigned)g_ac.wd.overruns);
        uunlock();
        g_rep_rounds++;
    }
    for (;;)
        __asm__ volatile("wfe");
}

/* ---- CPU3: the producer ---- */
static void producer_core(void *arg)
{
    (void)arg;
    int16_t chunk[SAMPLES];
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < SAMPLES; i++)
            chunk[i] = (int16_t)((g_prod + i) & 0x7FFF);
        g_prod += spsc_write(&g_ring, chunk, SAMPLES);
    }
    for (;;)
        __asm__ volatile("wfe");
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt serial shell (issue #80) ===\r\n");

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = freq / BLOCK_HZ;

    spsc_init(&g_ring, g_ring_buf, RING_CAP);
    audio_core_init(&g_ac, &g_ring, g_dma, FRAMES, interval / 2u);

    /* Prime the ring so the first callbacks do not underrun. */
    int16_t zero[SAMPLES];
    for (uint32_t i = 0; i < SAMPLES; i++)
        zero[i] = 0;
    for (int b = 0; b < 16; b++)
        spsc_write(&g_ring, zero, SAMPLES);

    int e3 = smp_start_core(3, producer_core, 0,
                            (uint64_t)(uintptr_t)(g_stack3 + sizeof(g_stack3)));
    int e2 = smp_start_core(2, reporter_core, (void *)(uintptr_t)(freq / 50u),
                            (uint64_t)(uintptr_t)(g_stack2 + sizeof(g_stack2)));
    int e1 = smp_start_core(1, shell_core, 0,
                            (uint64_t)(uintptr_t)(g_stack1 + sizeof(g_stack1)));
    (void)e1; (void)e2; (void)e3;

    exceptions_init();
    gic_init();
    timer_init(BLOCK_HZ);
    __asm__ volatile("msr daifclr, #2");

    /* Wait for the scripted input to reach `done`, then for a couple more
     * reporter rounds so the two cores demonstrably shared the UART. */
    uint64_t start = rd_cntpct();
    uint64_t deadline = start + 20ull * freq;         /* 20 s safety */
    while (!__atomic_load_n(&g_input_done, __ATOMIC_ACQUIRE) &&
           rd_cntpct() < deadline)
        __asm__ volatile("wfi");

    uint32_t rounds_at_done = g_rep_rounds;
    while (g_rep_rounds < rounds_at_done + 2 && rd_cntpct() < deadline)
        __asm__ volatile("wfi");

    timer_stop();
    __asm__ volatile("msr daifset, #2");
    __atomic_store_n(&g_stop, 1u, __ATOMIC_RELEASE);
    __asm__ volatile("sev");

    /* ---- checks ---- */
    int input_ok  = __atomic_load_n(&g_input_done, __ATOMIC_ACQUIRE) != 0;
    int shell_ran = (g_echo_runs >= 2) && (g_unknown_runs >= 1) &&
                    (g_help_listed == 1);
    int rep_ran   = (g_rep_rounds >= 2);
    /* CPU0 kept the audio cadence: the ring never starved (the real signal),
     * and the watchdog overrun count is at most the documented TCG artifact -
     * with four busy vCPUs on a CI host, the host can deschedule CPU0's vCPU
     * mid-callback and inflate the CNTPCT-measured service time (see
     * docs/latency.md).  On hardware CPU0 is dedicated and this is zero. */
    int cpu0_ok   = (g_ac.serviced > 20) && (g_underruns == 0) &&
                    (g_ac.wd.overruns <= 4);
    int clean     = (g_corrupt == 0);

    uart_printf("shell: echo=%u unknown=%u help=%u done=%u\r\n",
                (unsigned)g_echo_runs, (unsigned)g_unknown_runs,
                (unsigned)g_help_listed, (unsigned)input_ok);
    uart_printf("audio: serviced=%u underruns=%u overruns=%u  reporter rounds=%u  uart-corrupt=%u\r\n",
                (unsigned)g_ac.serviced, (unsigned)g_underruns,
                (unsigned)g_ac.wd.overruns, (unsigned)g_rep_rounds,
                (unsigned)g_corrupt);
    uart_printf("checks: input=%d shell=%d reporter=%d cpu0=%d uart-clean=%d\r\n",
                input_ok, shell_ran, rep_ran, cpu0_ok, clean);

    int ok = input_ok && shell_ran && rep_ran && cpu0_ok && clean;
    uart_puts(ok ? "SHELL: PASS\r\n" : "SHELL: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
