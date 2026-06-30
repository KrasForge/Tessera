/* tests/arm64/virt/plugin_host_main.c - resilient plugin host on QEMU 'virt'
 * (Issue #26, M5 capstone).
 *
 * Proves the milestone's "done when": an isolated plugin produces sound, and
 * killing the plugin process does not crash the host or stop audio output.
 *
 *   Phase 1 - a sine plugin loads into its own address space and fills the
 *             shared ring with a 440 Hz tone; the host drains every block and
 *             sees real (non-silent) audio.  The plugin produced sound.
 *   Phase 2 - a sine plugin writes two blocks and then crashes (writes to
 *             kernel memory).  The kernel kills it and stamps the ring's
 *             producer-status word DEAD.  The host drains the two real blocks,
 *             then detects the death, logs it once, and continues feeding
 *             silence - it neither crashes nor hangs.
 *
 * Samples are compared only as zero / non-zero (bit patterns), so this harness
 * needs no FP and stays -mgeneral-regs-only.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_host.h"
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char sine_elf_start[], sine_elf_end[];
extern char sine_crash_elf_start[], sine_crash_elf_end[];

/* host_on_death override: log the crash and state the restart policy. */
void host_on_death(plugin_host_t *h)
{
    (void)h;
    uart_puts("  [host] plugin producer DEAD: substituting silence (policy: continue)\r\n");
}

/* A block is "sound" if any sample word is non-zero; silence is all zero.
 * Samples are inspected as raw words so the harness needs no FP. */
static int block_has_sound(const uint32_t *w)
{
    for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
        if (w[i] != 0u)
            return 1;
    return 0;
}

static audio_ring_hdr_t *g_ring;
static uintptr_t g_ring_pa;
static size_t    g_ring_pages;

static int run_plugin(const char *name, char *elf, size_t len, int set_liveness)
{
    arb_init(g_ring, RING_FRAMES);
    plugin_t pl;
    if (plugin_load(&pl, elf, len, name) != PLUGIN_OK)
        return -100;
    plugin_map_region(&pl, RING_VA, g_ring_pa, g_ring_pages * PAGE_SIZE,
                      VMM_READ | VMM_WRITE);
    if (set_liveness)
        process_set_liveness(pl.proc, &g_ring->producer_state);
    return (int)plugin_call_init(&pl, RING_SR, RING_BLOCK);
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt resilient plugin host (issue #26) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    size_t bytes = arb_bytes(RING_FRAMES);
    g_ring_pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    g_ring_pa = phys_alloc_contig(g_ring_pages);
    g_ring = (audio_ring_hdr_t *)P2V(g_ring_pa);

    uint32_t out[RING_BLOCK * 2u];

    /* ---- Phase 1: the plugin produces sound ---- */
    int c1 = run_plugin("sine", sine_elf_start,
                        (size_t)(sine_elf_end - sine_elf_start), 0);
    plugin_host_t host;
    host_init(&host, g_ring, RING_BLOCK);

    uint32_t sound_blocks = 0, full_blocks = 0;
    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        int full = host_block(&host, (float *)out);
        if (full) full_blocks++;
        if (block_has_sound(out)) sound_blocks++;
    }
    uart_printf("phase1: init=%d full=%u/%u sound=%u overruns=%u\r\n",
                c1, (unsigned)full_blocks, (unsigned)RING_NBLOCKS,
                (unsigned)sound_blocks, (unsigned)host.overruns);
    int produced_sound = (c1 == TESSERA_PLUGIN_OK) &&
                         (full_blocks == RING_NBLOCKS) &&
                         (sound_blocks == RING_NBLOCKS) && (host.overruns == 0);

    /* ---- Phase 2: kill the plugin mid-stream, host survives ---- */
    int c2 = run_plugin("sine-crash", sine_crash_elf_start,
                        (size_t)(sine_crash_elf_end - sine_crash_elf_start), 1);
    host_init(&host, g_ring, RING_BLOCK);

    uint32_t real = 0, silent = 0;
    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        int full = host_block(&host, (float *)out);
        if (full && block_has_sound(out)) real++;
        else if (!block_has_sound(out)) silent++;
    }
    uart_printf("phase2: init=%d dead=%d real=%u silent=%u overruns=%u logged=%u\r\n",
                c2, host_producer_dead(&host), (unsigned)real, (unsigned)silent,
                (unsigned)host.overruns, (unsigned)host.dead_logged);

    int survived = (c2 == -1) && host_producer_dead(&host) &&
                   (real == 2u) && (silent == RING_NBLOCKS - 2u) &&
                   (host.overruns > 0) && (host.dead_logged == 1);

    uart_puts("  [host] reached end of run without crashing or hanging\r\n");
    uart_printf("checks: produced-sound=%d killed-survived=%d\r\n",
                produced_sound, survived);
    uart_puts((produced_sound && survived) ? "PLUGIN-HOST: PASS\r\n"
                                           : "PLUGIN-HOST: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
