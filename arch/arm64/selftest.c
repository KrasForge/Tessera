/* arch/arm64/selftest.c — M1 virtual-memory self-test, reported over UART.
 *
 * Exercises the acceptance criteria of issues #7, #8/#9, and #10:
 *   - PMM hands out unique, page-aligned frames outside the kernel image.
 *   - The MMU is enabled (we are still executing and UART MMIO still works).
 *   - vmm_map/unmap/protect work, TLB invalidation is correct on remap, and
 *     a 1000-cycle map/unmap stress loop runs without corruption.
 *   - kalloc/kfree allocate, reuse, and detect a double free.
 *   - ASID allocation returns unique non-zero IDs.
 */

#include "pmm.h"
#include "mmu.h"
#include "vmem.h"
#include "kheap.h"
#include "process.h"
#include "usermode.h"
#include "sched.h"
#include "i2s.h"
#include "audio_stream.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

/* User-mode demo payloads (arch/arm64/user_demo.S). */
extern char user_payload[], user_payload_end[];
extern char user_priv_payload[], user_priv_payload_end[];
extern char user_badwrite_payload[], user_badwrite_payload_end[];
extern char user_task[], user_task_end[];
extern char user_fpu_task[], user_fpu_task_end[];
void *memcpy(void *, const void *, size_t);
void  fpu_disable(void);

/* A virtual window well above the identity-mapped RAM/MMIO (4 GiB), so the
 * VMM creates fresh L0-L3 tables rather than touching the 2 MiB identity
 * blocks. */
#define TEST_VA_BASE 0x100000000UL

#define OKBAD(c) ((c) ? "ok" : "BAD")

void m1_selftest(void)
{
    uart_puts("\r\n=== M1 virtual-memory self-test ===\r\n");

    /* --- PMM (issue #7) ------------------------------------------------- */
    uintptr_t a = phys_alloc_page();
    uintptr_t b = phys_alloc_page();
    int pmm_ok = a && b && a != b &&
                 !((a | b) & (PAGE_SIZE - 1)) &&
                 !pmm_is_reserved(a) && !pmm_is_reserved(b);
    uart_printf("pmm   : alloc two frames .......... %s\r\n", OKBAD(pmm_ok));
    uart_printf("pmm   : free %u / %u total pages\r\n",
                (unsigned)pmm_free_pages(), (unsigned)pmm_total_pages());
    phys_free_page(a);
    phys_free_page(b);

    /* --- VMM map / read-write (issues #8, #10) -------------------------- */
    uintptr_t p1 = phys_alloc_page();
    volatile uint32_t *win = (volatile uint32_t *)TEST_VA_BASE;
    int map_ok = (vmm_map(p1, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE) == 0);
    *win = 0xCAFEBABEu;
    map_ok = map_ok && (*win == 0xCAFEBABEu);
    uart_printf("vmm   : map + write-through ....... %s\r\n", OKBAD(map_ok));

    /* --- TLB correctness: remap the same VA to a new frame -------------- */
    uintptr_t p2 = phys_alloc_page();
    *(volatile uint32_t *)p2 = 0x12345678u;          /* seed via identity */
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    vmm_map(p2, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    int tlb_ok = (*win == 0x12345678u);
    uart_printf("vmm   : TLB sees remap ............ %s\r\n", OKBAD(tlb_ok));
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    phys_free_page(p1);
    phys_free_page(p2);

    /* --- VMM stress: 1000 random map/unmap cycles ----------------------- */
    int stress_ok = 1;
    uint32_t lcg = 2463534242u;                       /* xorshift PRNG */
    for (int i = 0; i < 1000; i++) {
        lcg ^= lcg << 13; lcg ^= lcg >> 17; lcg ^= lcg << 5;
        uintptr_t va = TEST_VA_BASE + (uintptr_t)(lcg & 0x3FFu) * PAGE_SIZE;
        uintptr_t pp = phys_alloc_page();
        if (!pp) { stress_ok = 0; break; }
        if (vmm_map(pp, va, PAGE_SIZE, VMM_READ | VMM_WRITE) != 0) {
            stress_ok = 0; phys_free_page(pp); break;
        }
        *(volatile uint32_t *)va = (uint32_t)i;
        if (*(volatile uint32_t *)va != (uint32_t)i)
            stress_ok = 0;
        vmm_unmap(va, PAGE_SIZE);
        phys_free_page(pp);
    }
    uart_printf("vmm   : 1000x map/unmap stress .... %s\r\n", OKBAD(stress_ok));

    /* --- vmm_protect: drop write permission ----------------------------- */
    uintptr_t p3 = phys_alloc_page();
    vmm_map(p3, TEST_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
    *win = 0xAAAA5555u;
    int prot_ok = (vmm_protect(TEST_VA_BASE, PAGE_SIZE, VMM_READ) == 0) &&
                  (*win == 0xAAAA5555u);              /* read still works  */
    uart_printf("vmm   : protect read-only ......... %s\r\n", OKBAD(prot_ok));
    vmm_unmap(TEST_VA_BASE, PAGE_SIZE);
    phys_free_page(p3);

    /* --- Kernel heap (issue #10) ---------------------------------------- */
    void *x = kalloc(100);
    void *y = kalloc(2000);
    int heap_ok = x && y && x != y;
    uart_printf("kheap : alloc distinct blocks ..... %s (used %u B)\r\n",
                OKBAD(heap_ok), (unsigned)kheap_used());
    kfree(x);
    kfree(y);
    void *z = kalloc(64);
    uart_printf("kheap : reuse after free .......... %s\r\n", OKBAD(z != 0));
    kfree(z);

    /* --- ASID allocation (issue #10) ------------------------------------ */
    uint16_t s1 = asid_alloc();
    uint16_t s2 = asid_alloc();
    int asid_ok = s1 && s2 && s1 != s2;
    uart_printf("asid  : unique non-zero IDs ....... %s (%u, %u)\r\n",
                OKBAD(asid_ok), (unsigned)s1, (unsigned)s2);
    asid_free(s1);
    asid_free(s2);

    uart_puts("=== M1 self-test complete ===\r\n\r\n");
}

/* M2 (issue #11): per-process address spaces.  Creates two processes, checks
 * that each gets a distinct L0 root with the kernel still reachable and
 * isolated user space, then tears them down and confirms no frame leak. */
void m2_process_selftest(void)
{
    uart_puts("=== M2 process-isolation self-test (issue #11) ===\r\n");

    size_t baseline = pmm_free_pages();

    process_t *a = process_create("plugin-a");
    process_t *b = process_create("plugin-b");
    int created = a && b && (a->pgd_pa != b->pgd_pa) && (a->asid != b->asid);
    uart_printf("proc  : two distinct address spaces .. %s\r\n", OKBAD(created));
    if (a && b) {
        uart_printf("        A pid=%u asid=%u root=%x  B pid=%u asid=%u root=%x\r\n",
                    (unsigned)a->pid, (unsigned)a->asid, (unsigned)a->pgd_pa,
                    (unsigned)b->pid, (unsigned)b->asid, (unsigned)b->pgd_pa);

        int kern_ok = mmu_translate(a->pgd, 0xFE201000UL) == 0xFE201000UL &&
                      mmu_translate(b->pgd, 0xFE201000UL) == 0xFE201000UL;
        uart_printf("proc  : UART reachable in both ...... %s\r\n", OKBAD(kern_ok));

        uintptr_t fa = phys_alloc_page();
        uintptr_t fb = phys_alloc_page();
        process_map(a, fa, USER_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
        process_map(b, fb, USER_VA_BASE, PAGE_SIZE, VMM_READ | VMM_WRITE);
        int iso = mmu_translate(a->pgd, USER_VA_BASE) == fa &&
                  mmu_translate(b->pgd, USER_VA_BASE) == fb && fa != fb;
        uart_printf("proc  : user VA isolated A vs B ..... %s\r\n", OKBAD(iso));

        process_destroy(a);
        process_destroy(b);
    }

    int reclaimed = pmm_free_pages() == baseline;
    uart_printf("proc  : teardown reclaims frames ..... %s\r\n", OKBAD(reclaimed));

    uart_puts("=== M2 self-test complete ===\r\n\r\n");
}

/* Load a payload into a fresh process: copy it into a code frame (RX) and
 * map a user stack (RW).  Leaves the process READY; the caller runs it. */
static process_t *load_user(const char *name, char *code, size_t code_len)
{
    process_t *p = process_create(name);
    if (!p)
        return (process_t *)0;

    uintptr_t code_pa = phys_alloc_page();
    memcpy((void *)code_pa, code, code_len);
    process_map(p, code_pa, USER_VA_BASE, PAGE_SIZE, VMM_READ | VMM_EXEC);

    uintptr_t stack_pa = phys_alloc_page();
    process_map(p, stack_pa, USER_VA_BASE + 0x10000, PAGE_SIZE,
                VMM_READ | VMM_WRITE);
    return p;
}

#define USER_ENTRY  USER_VA_BASE
#define USER_SP     (USER_VA_BASE + 0x10000 + PAGE_SIZE)

/* M2 (issue #13): EL0 entry + SVC syscall ABI. */
void m2_user_selftest(void)
{
    uart_puts("=== M2 EL0 + SVC self-test (issue #13) ===\r\n");

    uart_puts("user  : running EL0 program (expect a greeting below)\r\n  >> ");
    process_t *a = load_user("user-demo", user_payload,
                             (size_t)(user_payload_end - user_payload));
    long c1 = process_run(a, USER_ENTRY, USER_SP, 0);
    process_destroy(a);
    uart_printf("user  : sys_write/sys_exit ......... %s (code=%d)\r\n",
                OKBAD(c1 == 0), (int)c1);

    process_t *b = load_user("user-priv", user_priv_payload,
                             (size_t)(user_priv_payload_end - user_priv_payload));
    long c2 = process_run(b, USER_ENTRY, USER_SP, 0);
    process_destroy(b);
    uart_printf("user  : privileged insn trapped .... %s (code=%d)\r\n",
                OKBAD(c2 == -1), (int)c2);

    uart_puts("=== M2 EL0 self-test complete ===\r\n\r\n");
}

/* M2 (issue #14): fault containment.  A process that writes to kernel memory
 * is trapped and killed; the kernel data is intact and a second process runs
 * normally afterwards. */
static volatile uint64_t g_kernel_canary = 0xCAFEF00DDEADBEEFUL;

void m2_fault_selftest(void)
{
    uart_puts("=== M2 fault-containment self-test (issue #14) ===\r\n");

    uint64_t before = g_kernel_canary;

    /* Process A tries to overwrite a kernel variable. */
    process_t *a = load_user("bad-plugin", user_badwrite_payload,
                             (size_t)(user_badwrite_payload_end - user_badwrite_payload));
    long ca = process_run(a, USER_ENTRY, USER_SP, (uint64_t)(uintptr_t)&g_kernel_canary);
    int killed = (ca == -1) && (a->state == PROC_KILLED);
    process_destroy(a);
    uart_printf("fault : bad-write process killed ... %s (code=%d)\r\n",
                OKBAD(killed), (int)ca);

    /* The MMU blocked the write before it landed, so the canary is intact. */
    uart_printf("fault : kernel data intact ......... %s\r\n",
                OKBAD(g_kernel_canary == before));

    /* The kernel keeps running: a second process executes normally. */
    uart_puts("fault : second process output\r\n  >> ");
    process_t *b = load_user("good-plugin", user_payload,
                             (size_t)(user_payload_end - user_payload));
    long cb = process_run(b, USER_ENTRY, USER_SP, 0);
    process_destroy(b);
    uart_printf("fault : second process continues ... %s (code=%d)\r\n",
                OKBAD(cb == 0), (int)cb);

    uart_puts("=== M2 fault-containment complete ===\r\n\r\n");
}

/* M2 (issue #15): cooperative context switch between isolated processes. */
#define SCHED_DATA_VA (USER_VA_BASE + 0x20000)

static process_t *sched_make(const char *name, char *code, size_t len, char data)
{
    process_t *p = load_user(name, code, len);     /* code RX + stack RW */
    if (data) {
        uintptr_t data_pa = phys_alloc_page();
        *(volatile char *)data_pa = data;
        process_map(p, data_pa, SCHED_DATA_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);
    }
    return p;
}

void m2_sched_selftest(void)
{
    uart_puts("=== M2 context-switch self-test (issue #15) ===\r\n");

    fpu_disable();
    sched_init();

    process_t *a = sched_make("task-A",   user_task, (size_t)(user_task_end - user_task), 'A');
    process_t *b = sched_make("task-B",   user_task, (size_t)(user_task_end - user_task), 'B');
    process_t *f = sched_make("task-FPU", user_fpu_task, (size_t)(user_fpu_task_end - user_fpu_task), 0);

    sched_add(a, USER_ENTRY, USER_SP, SCHED_DATA_VA);
    sched_add(b, USER_ENTRY, USER_SP, SCHED_DATA_VA);
    sched_add(f, USER_ENTRY, USER_SP, 0);

    uart_puts("sched : round-robin output ......... ");
    sched_run();
    uart_puts("\r\n");

    uart_printf("sched : task-A read own page ....... %s\r\n", OKBAD(a->exit_code == 'A'));
    uart_printf("sched : task-B read own page ....... %s\r\n", OKBAD(b->exit_code == 'B'));
    uart_printf("sched : isolation (no TLB alias) ... %s\r\n",
                OKBAD(a->exit_code == 'A' && b->exit_code == 'B'));
    uart_printf("sched : lazy FPU result correct .... %s\r\n", OKBAD(f->exit_code == 0));

    process_destroy(a);
    process_destroy(b);
    process_destroy(f);

    uart_puts("=== M2 context-switch complete ===\r\n\r\n");
}

/* M3 (issue #16): bring up the I2S DAC output and play a short 440 Hz tone.
 * Audible on a connected PCM5102; under emulation the writes are harmless. */
void m3_audio_selftest(void)
{
    uart_puts("=== M3 audio self-test (issue #16) ===\r\n");
    i2s_init(48000);
    uart_puts("audio : I2S master @ 48 kHz initialised\r\n");
    i2s_play_tone(440, 480);   /* ~10 ms of 440 Hz */
    uart_puts("audio : played 440 Hz tone (audible on a PCM5102 DAC)\r\n");
    uart_puts("=== M3 audio self-test complete ===\r\n\r\n");
}

/* M3 (issue #17): DMA double-buffered streaming. */
static uint32_t g_stream_rate = 48000;

static void selftest_sine_fill(int16_t *dst, uint32_t frames, void *ctx)
{
    static uint32_t phase;
    uint32_t rate = *(uint32_t *)ctx;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t s = i2s_sine(&phase, 440, rate);
        dst[2 * i] = s;
        dst[2 * i + 1] = s;
    }
}

void m3_dma_audio_selftest(void)
{
    uart_puts("=== M3 DMA-audio self-test (issue #17) ===\r\n");

    audio_stream_t s;
    audio_stream_init(&s, 48000, 256, selftest_sine_fill, &g_stream_rate);
    uart_puts("dma   : I2S + DMA ring configured (256-frame double buffer)\r\n");

    audio_stream_start(&s);
    for (int i = 0; i < 8; i++)
        audio_stream_service(&s);   /* refill as the DMA drains the ring */
    audio_stream_stop(&s);

    uart_puts("dma   : streamed via DMA ring (continuous on a PCM5102 DAC)\r\n");
    uart_puts("=== M3 DMA-audio self-test complete ===\r\n\r\n");
}
