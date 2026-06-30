/* tests/arm64/virt/graph_main.c - audio graph processing on QEMU 'virt'
 * (Issue #27, M6).
 *
 * Drives real isolated plugins through ring-buffer edges in the order the graph
 * model computes:
 *
 *   2 nodes:  synth -> DAC          (same shape as M5)
 *   3 nodes:  synth -> effect -> DAC
 *
 * The kernel allocates a ring per edge and maps it into the producing plugin's
 * output VA and the consuming plugin's input VA.  Walking the topological order
 * (synth before effect before DAC) makes every node see filled inputs; the DAC
 * (host) then drains real audio.  As a negative control, running the 3-node
 * graph in the WRONG order leaves the DAC with silence - proving the order is
 * what makes it work.
 *
 * Samples are inspected as raw words, so the harness needs no FP.
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
#include "audio_graph.h"
#include "ring_contract.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char synth_elf_start[], synth_elf_end[];
extern char effect_elf_start[], effect_elf_end[];

#define SYNTH_PID  1u
#define EFFECT_PID 2u

static int pid_ok(uint32_t pid) { return pid == SYNTH_PID || pid == EFFECT_PID; }

/* A ring edge: contiguous pages, kernel-visible header. */
typedef struct { uintptr_t pa; size_t pages; audio_ring_hdr_t *hdr; } edge_t;

static edge_t make_edge(void)
{
    edge_t e;
    size_t bytes = arb_bytes(RING_FRAMES);
    e.pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    e.pa    = phys_alloc_contig(e.pages);
    e.hdr   = (audio_ring_hdr_t *)P2V(e.pa);
    arb_init(e.hdr, RING_FRAMES);
    return e;
}

/* Load a plugin and map its output/input edges (NULL to skip), then run it. */
static long run_node(char *elf, size_t len, const char *name,
                     edge_t *out_edge, edge_t *in_edge)
{
    plugin_t pl;
    if (plugin_load(&pl, elf, len, name) != PLUGIN_OK)
        return -100;
    if (out_edge)
        plugin_map_region(&pl, RING_VA, out_edge->pa,
                          out_edge->pages * PAGE_SIZE, VMM_READ | VMM_WRITE);
    if (in_edge)
        plugin_map_region(&pl, RING_IN_VA, in_edge->pa,
                          in_edge->pages * PAGE_SIZE, VMM_READ | VMM_WRITE);
    return plugin_call_init(&pl, RING_SR, RING_BLOCK);
}

/* Drain `edge` as the DAC sink would; return how many of RING_NBLOCKS blocks
 * carried sound (any non-zero sample word). */
static uint32_t dac_drain(edge_t *edge)
{
    uint32_t out[RING_BLOCK * 2u];
    uint32_t sound = 0;
    for (uint32_t b = 0; b < RING_NBLOCKS; b++) {
        arb_read(edge->hdr, (float *)out, RING_BLOCK);
        for (uint32_t i = 0; i < RING_BLOCK * 2u; i++)
            if (out[i] != 0u) { sound++; break; }
    }
    return sound;
}

/* Build a graph, sort it, and report the node order as a type string. */
static int sorted_types(const audio_graph_t *g, char *buf, int max)
{
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(g, order, GRAPH_MAX_NODES);
    int k = 0;
    for (int i = 0; i < n && k < max - 1; i++) {
        node_type_t t = g->nodes[order[i]].type;
        buf[k++] = (t == NODE_DAC) ? 'D' : (g->nodes[order[i]].pid == SYNTH_PID ? 'S' : 'E');
    }
    buf[k] = '\0';
    return n;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt audio graph (issue #27) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    size_t synth_len = (size_t)(synth_elf_end - synth_elf_start);
    size_t effect_len = (size_t)(effect_elf_end - effect_elf_start);

    /* ---- 2-node graph: synth -> DAC ---- */
    audio_graph_t g2;
    audio_graph_init(&g2, pid_ok);
    int s2 = audio_graph_add_node(&g2, SYNTH_PID);
    int d2 = audio_graph_add_dac(&g2);
    audio_graph_connect(&g2, s2, d2);
    char ord2[8];
    sorted_types(&g2, ord2, sizeof(ord2));
    uart_printf("2-node order = %s (expect SD)\r\n", ord2);

    edge_t e1 = make_edge();
    run_node(synth_elf_start, synth_len, "synth", &e1, 0);
    uint32_t snd2 = dac_drain(&e1);
    uart_printf("2-node DAC sound blocks = %u/%u\r\n", (unsigned)snd2, (unsigned)RING_NBLOCKS);
    int two_ok = (ord2[0] == 'S' && ord2[1] == 'D') && (snd2 == RING_NBLOCKS);

    /* ---- 3-node graph: synth -> effect -> DAC, correct order ---- */
    audio_graph_t g3;
    audio_graph_init(&g3, pid_ok);
    int s3 = audio_graph_add_node(&g3, SYNTH_PID);
    int ef = audio_graph_add_node(&g3, EFFECT_PID);
    int d3 = audio_graph_add_dac(&g3);
    audio_graph_connect(&g3, s3, ef);
    audio_graph_connect(&g3, ef, d3);
    char ord3[8];
    sorted_types(&g3, ord3, sizeof(ord3));
    uart_printf("3-node order = %s (expect SED)\r\n", ord3);

    edge_t a = make_edge();   /* synth -> effect */
    edge_t b = make_edge();   /* effect -> DAC   */
    run_node(synth_elf_start, synth_len, "synth", &a, 0);     /* node S */
    run_node(effect_elf_start, effect_len, "effect", &b, &a); /* node E */
    uint32_t snd3 = dac_drain(&b);                            /* node D */
    uart_printf("3-node DAC sound blocks = %u/%u\r\n", (unsigned)snd3, (unsigned)RING_NBLOCKS);
    int three_ok = (ord3[0] == 'S' && ord3[1] == 'E' && ord3[2] == 'D') &&
                   (snd3 == RING_NBLOCKS);

    /* ---- negative control: WRONG order leaves the DAC silent ---- */
    edge_t a2 = make_edge();
    edge_t b2 = make_edge();
    run_node(effect_elf_start, effect_len, "effect", &b2, &a2); /* E before S */
    run_node(synth_elf_start, synth_len, "synth", &a2, 0);
    uint32_t sndw = dac_drain(&b2);
    uart_printf("wrong-order DAC sound blocks = %u (expect 0)\r\n", (unsigned)sndw);
    int control_ok = (sndw == 0);

    uart_printf("checks: two=%d three=%d order-matters=%d\r\n",
                two_ok, three_ok, control_ok);
    uart_puts((two_ok && three_ok && control_ok) ? "AUDIO-GRAPH: PASS\r\n"
                                                 : "AUDIO-GRAPH: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
