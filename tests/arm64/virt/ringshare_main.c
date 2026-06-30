/* tests/arm64/virt/ringshare_main.c - shared-memory ring buffer between host
 * and an isolated plugin on QEMU 'virt' (Issue #25, M5).
 *
 * The kernel allocates one physically-contiguous ring, maps it read-write into
 * a plugin's address space at RING_VA, and keeps its own (identity) view.  A
 * producer plugin writes 8 blocks of 256 stereo float frames straight into the
 * ring with no syscalls; the host then drains them and checks the ramp - audio
 * crossed an isolation boundary with zero kernel calls per block (a SVC counter
 * proves the only syscall was the plugin's final exit).
 *
 * A second plugin writes two blocks and then crashes; the host drains the valid
 * frames and fills the rest with silence, proving the buffer survives a plugin
 * crash.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char producer_elf_start[], producer_elf_end[];
extern char crasher_elf_start[], crasher_elf_end[];

/* SVC counter (overrides the weak hook in syscalls.c). */
static volatile uint32_t g_svc;
void syscall_trace(uint64_t num) { (void)num; g_svc++; }

static void enable_fp(void)
{
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ull << 20);
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(cpacr));
}

static int feq(float a, float b)
{
    float d = a - b;
    return d < 0.01f && d > -0.01f;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt shared-memory ring buffer (issue #25) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();
    enable_fp();

    /* One contiguous ring, kernel-visible via its identity PA. */
    size_t bytes = arb_bytes(RING_FRAMES);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t ring_pa = phys_alloc_contig(pages);
    audio_ring_hdr_t *ring = (audio_ring_hdr_t *)P2V(ring_pa);
    arb_init(ring, RING_FRAMES);
    uart_printf("ring: %u frames, %u bytes, %u pages at PA 0x%x\r\n",
                (unsigned)RING_FRAMES, (unsigned)bytes, (unsigned)pages,
                (unsigned)ring_pa);

    /* ---- producer plugin: write 8 blocks with zero syscalls ---- */
    plugin_t prod;
    int lr = plugin_load(&prod, producer_elf_start,
                         (size_t)(producer_elf_end - producer_elf_start), "producer");
    plugin_map_region(&prod, RING_VA, ring_pa, pages * PAGE_SIZE,
                      VMM_READ | VMM_WRITE);

    g_svc = 0;
    long code = plugin_call_init(&prod, RING_SR, RING_BLOCK);
    uint32_t prod_svcs = g_svc;
    uart_printf("producer init -> %d, SVCs during run = %u (1 = only the exit)\r\n",
                (int)code, (unsigned)prod_svcs);
    uart_printf("ring after producer: write=%u read=%u overflow=%u\r\n",
                ring->write_idx, ring->read_idx, ring->overflow);

    /* Host drains and verifies the ramp, block by block. */
    float out[RING_BLOCK * 2u];
    uint32_t total = 0;
    int ramp_ok = 1;
    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        uint32_t got = arb_read(ring, out, RING_BLOCK);
        if (got != RING_BLOCK) ramp_ok = 0;
        for (uint32_t i = 0; i < got; i++) {
            float v = (float)(total + i);
            if (!feq(out[i * 2], v) || !feq(out[i * 2 + 1], v + 0.5f)) ramp_ok = 0;
        }
        total += got;
    }
    uart_printf("host drained %u frames, ramp_ok=%d\r\n", (unsigned)total, ramp_ok);

    int producer_ok = (lr == PLUGIN_OK) && (code == TESSERA_PLUGIN_OK) &&
                      (prod_svcs == 1) && (total == RING_FRAMES) && ramp_ok;

    /* ---- crasher plugin: survives a mid-stream crash ---- */
    arb_init(ring, RING_FRAMES);                  /* fresh ring */
    plugin_t crash;
    int cr = plugin_load(&crash, crasher_elf_start,
                         (size_t)(crasher_elf_end - crasher_elf_start), "crasher");
    plugin_map_region(&crash, RING_VA, ring_pa, pages * PAGE_SIZE,
                      VMM_READ | VMM_WRITE);
    long ccode = plugin_call_init(&crash, RING_SR, RING_BLOCK);
    uart_printf("crasher init -> %d, state=%d (4=KILLED), wrote %u frames\r\n",
                (int)ccode, crash.proc ? (int)crash.proc->state : -1,
                ring->write_idx);

    /* Host drains the whole ring window: valid frames then silence. */
    uint32_t real = 0, silent = 0;
    int partial_ok = 1;
    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        uint32_t got = arb_read(ring, out, RING_BLOCK);
        for (uint32_t i = 0; i < RING_BLOCK; i++) {
            if (i < got) {
                float v = (float)(real + i);
                if (!feq(out[i * 2], v)) partial_ok = 0;
            } else {
                if (out[i * 2] != 0.0f || out[i * 2 + 1] != 0.0f) partial_ok = 0;
                silent++;
            }
        }
        real += got;
    }
    uart_printf("after crash: real=%u silent=%u underflow=%u partial_ok=%d\r\n",
                (unsigned)real, (unsigned)silent, ring->underflow, partial_ok);

    int crash_ok = (cr == PLUGIN_OK) && (ccode == -1) &&
                   (crash.proc->state == PROC_KILLED) &&
                   (real == 2u * RING_BLOCK) && (silent > 0) &&
                   (ring->underflow > 0) && partial_ok;

    uart_printf("checks: producer=%d crash-survive=%d\r\n", producer_ok, crash_ok);
    uart_puts((producer_ok && crash_ok) ? "RING-SHARE: PASS\r\n"
                                        : "RING-SHARE: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
