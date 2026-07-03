/* tests/arm64/virt/graph_input_main.c - the capture input node end to end on
 * QEMU 'virt' (Issue #84).
 *
 * The M14 acceptance for the input node, driven through the control plane
 * (pm_* / graph_control), MMU on, single core:
 *
 *   A. input -> filter -> DAC carries audio: the input node pulls a captured
 *      block from the I2S capture ring (issue #83), the low-pass filter
 *      processes it, and the DAC reads a non-silent result.
 *   B. bypass is bit-exact: wired input -> DAC directly, the DAC block equals
 *      the captured input block sample-for-sample.
 *   C. a patch containing the input node saves, reloads, and rebuilds the
 *      identical graph (input -> filter -> DAC), through the text patch on a
 *      FAT SD card.
 *   D. underrun: with the capture ring empty, the input node emits silence and
 *      counts it, so downstream always gets a full block.
 *
 * The graph is walked in toposort order; the input node is just another
 * source, except it reads the capture ring instead of running a plugin.  Node
 * I/O pages are the plugin-native de-interleaved float format, but this
 * (kernel-side) harness never does floating point: it moves pages as raw
 * words and defers int16<->float conversion to gi_conv.c.  A block is `frames`
 * = RING_BLOCK stereo pairs, matching the plugin page layout.
 *
 * Built MMU-on (virt_mmu.ld) with the virt GIC bases; single core.
 */

#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_mgr.h"
#include "graph_control.h"
#include "audio_graph.h"
#include "ring_contract.h"
#include "i2s_capture.h"
#include "fat.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

/* FP-compiled conversion helpers (gi_conv.c). */
void gi_capture_to_page(void *page, const int16_t *interleaved, uint32_t frames);
int  gi_page_matches_capture(const void *page, const int16_t *interleaved,
                             uint32_t frames);

extern char effect_elf_start[], effect_elf_end[];

/* ---- in-RAM FAT16 SD card (for the patch round-trip) ---- */
#define SD_SECTORS 64
static uint8_t g_sd[SD_SECTORS * FAT_SECTOR];
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void sd_format(void)
{
    for (size_t i = 0; i < sizeof(g_sd); i++) g_sd[i] = 0;
    uint8_t *b = g_sd;
    put16(b + 0x0B, 512); b[0x0D] = 1; put16(b + 0x0E, 1);
    b[0x10] = 1; put16(b + 0x11, 16); put16(b + 0x16, 1);
    b[510] = 0x55; b[511] = 0xAA;
}
static int sd_read(void *c, uint32_t lba, uint8_t *buf)
{ (void)c; if (lba >= SD_SECTORS) return -1;
  for (uint32_t i = 0; i < FAT_SECTOR; i++) buf[i] = g_sd[lba * FAT_SECTOR + i]; return 0; }
static int sd_write(void *c, uint32_t lba, const uint8_t *buf)
{ (void)c; if (lba >= SD_SECTORS) return -1;
  for (uint32_t i = 0; i < FAT_SECTOR; i++) g_sd[lba * FAT_SECTOR + i] = buf[i]; return 0; }

/* ---- ring backend for the control plane (a real page per edge) ---- */
static void *ring_new(void *c)
{ (void)c; uintptr_t pa = phys_alloc_page_zero(); return pa ? P2V(pa) : (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;
static fat_fs_t        g_fat;

/* ---- the capture ring, fed by a modelled source ---- */
#define FRAMES   RING_BLOCK              /* match the plugin page layout    */
#define SAMPLES  (FRAMES * 2u)
#define NBLOCKS  4u
static int16_t       g_cap_store[NBLOCKS * SAMPLES];
static i2s_capture_t g_cap;

static int16_t src_sample(uint32_t seq, uint32_t i)
{
    return (int16_t)(((seq * 137u + i * 3u) & 0x7FFF) - 4096);
}
static void make_source(int16_t *blk, uint32_t seq)
{
    for (uint32_t i = 0; i < SAMPLES; i++)
        blk[i] = src_sample(seq, i);
}
static void feed_capture(uint32_t seq)
{
    int16_t blk[SAMPLES];
    make_source(blk, seq);
    i2s_capture_produce(&g_cap, blk);
}

/* ---- per-plugin / input I/O pages (raw words; FP happens only in plugins) ---- */
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

typedef struct { uint32_t pid; plugin_t *pl; uint32_t *out_k; uint32_t *in_k; } io_t;
static io_t g_io[GRAPH_MAX_NODES];
static int  g_n_io;
static uint32_t g_input_out[SAMPLES];    /* the input node's output page (float bits) */

static io_t *io_by_pid(uint32_t pid)
{
    for (int i = 0; i < g_n_io; i++)
        if (g_io[i].pid == pid) return &g_io[i];
    return (io_t *)0;
}

static void setup_plugin(uint32_t pid)
{
    plugin_t *pl = pm_plugin(&g_pm, pid);
    if (!pl) return;
    io_t *e = &g_io[g_n_io++];
    e->pid = pid; e->pl = pl;
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    e->out_k = (uint32_t *)P2V(out_pa);
    e->in_k  = (uint32_t *)P2V(in_pa);
    plugin_call_init(pl, RING_SR, RING_BLOCK);
}

static void setup_input(void)
{
    io_t *e = &g_io[g_n_io++];
    e->pid = GRAPH_INPUT_PID; e->pl = 0;
    e->out_k = g_input_out; e->in_k = 0;
}

static void page_clear(uint32_t *p) { for (uint32_t i = 0; i < SAMPLES; i++) p[i] = 0; }
static void page_copy(uint32_t *d, const uint32_t *s) { for (uint32_t i = 0; i < SAMPLES; i++) d[i] = s[i]; }
static int  page_has_sound(const uint32_t *p) { if (!p) return 0; for (uint32_t i = 0; i < SAMPLES; i++) if (p[i]) return 1; return 0; }

/* ---- one graph block in toposort order ---- */
static void run_block(void)
{
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g_gc.graph, order, GRAPH_MAX_NODES);
    if (n < 0) return;

    for (int i = 0; i < n; i++) {
        int idx = order[i];
        uint32_t pid = g_gc.graph.nodes[idx].pid;

        if (g_gc.graph.nodes[idx].type == NODE_INPUT) {
            int16_t blk[SAMPLES];
            i2s_capture_consume(&g_cap, blk);        /* silence + count on empty */
            io_t *in = io_by_pid(GRAPH_INPUT_PID);
            gi_capture_to_page(in->out_k, blk, FRAMES);
            continue;
        }
        if (g_gc.graph.nodes[idx].type == NODE_DAC)
            continue;

        io_t *me = io_by_pid(pid);
        if (!me) continue;
        page_clear(me->in_k);
        for (int e = 0; e < GRAPH_MAX_EDGES; e++)
            if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                if (u) page_copy(me->in_k, u->out_k);
            }
        plugin_call_block(me->pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
    }
}

static const uint32_t *dac_input(void)
{
    int dac = g_gc.graph.dac_node;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == dac) {
            io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
            if (u) return u->out_k;
        }
    return 0;
}

static int count_edges(void)
{
    int n = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++) if (g_gc.graph.edges[e].used) n++;
    return n;
}
static int has_edge_pid(uint32_t sp, uint32_t dp)
{
    int s = audio_graph_node_by_pid(&g_gc.graph, sp);
    int d = audio_graph_node_by_pid(&g_gc.graph, dp);
    return (s >= 0 && d >= 0) ? (audio_graph_find_edge(&g_gc.graph, s, d) >= 0) : 0;
}

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt graph input node (issue #84) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    sd_format();
    fat_mount(&g_fat, sd_read, 0);
    fat_set_writer(&g_fat, sd_write);

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    pm_mount_sd(&g_pm, &g_fat);
    gc_add_dac(&g_gc);
    gc_add_input(&g_gc);                         /* the capture source singleton */
    setup_input();
    pm_register_blob(&g_pm, "effect", effect_elf_start,
                     (size_t)(effect_elf_end - effect_elf_start));

    i2s_capture_init(&g_cap, g_cap_store, NBLOCKS, FRAMES);

    long fp = pm_load(&g_pm, "effect");
    setup_plugin((uint32_t)fp);

    /* ---- A. input -> filter -> DAC carries audio ---- */
    pm_connect(&g_pm, GRAPH_INPUT_PID, (uint32_t)fp);
    pm_connect(&g_pm, (uint32_t)fp, 0u);         /* filter -> DAC */
    uint32_t sound = 0;
    for (uint32_t seq = 0; seq < 8; seq++) {
        feed_capture(seq);
        run_block();
        if (page_has_sound(dac_input())) sound++;
    }
    int chain_ok = (sound == 8);
    uart_printf("A: input->filter->DAC sound-blocks=%u/8\r\n", (unsigned)sound);

    /* ---- B. bypass (input -> DAC) is bit-exact ---- */
    pm_disconnect(&g_pm, GRAPH_INPUT_PID, (uint32_t)fp);
    pm_disconnect(&g_pm, (uint32_t)fp, 0u);
    pm_connect(&g_pm, GRAPH_INPUT_PID, 0u);      /* input -> DAC */
    int exact = 1;
    for (uint32_t seq = 50; seq < 58; seq++) {
        feed_capture(seq);
        run_block();
        int16_t expect[SAMPLES];
        make_source(expect, seq);
        if (!gi_page_matches_capture(dac_input(), expect, FRAMES)) exact = 0;
    }
    uart_printf("B: bypass bit-exact=%d\r\n", exact);

    /* ---- D. underrun: empty ring -> silence downstream ---- */
    uint64_t under0 = g_cap.underruns;
    run_block();                                 /* no feed: ring empty */
    int under_ok = !page_has_sound(dac_input()) && (g_cap.underruns == under0 + 1);
    uart_printf("D: underrun-silent=%d underruns=%u\r\n",
                under_ok, (unsigned)g_cap.underruns);

    /* ---- C. patch round-trip with the input node ---- */
    pm_disconnect(&g_pm, GRAPH_INPUT_PID, 0u);
    pm_connect(&g_pm, GRAPH_INPUT_PID, (uint32_t)fp);
    pm_connect(&g_pm, (uint32_t)fp, 0u);         /* rebuild input->filter->DAC */

    long sv = pm_patch_save(&g_pm, "/sd/in.patch");
    uint8_t txt[512];
    long tn = fat_read_file(&g_fat, "in.patch", txt, sizeof(txt) - 1);
    int has_input_line = 0;
    if (tn > 0) {
        txt[tn] = '\0';
        for (long k = 0; k + 13 <= tn; k++)
            if (txt[k]=='c'&&txt[k+1]=='o'&&txt[k+8]=='i'&&txt[k+9]=='n'&&
                txt[k+10]=='p'&&txt[k+11]=='u'&&txt[k+12]=='t') { has_input_line = 1; break; }
        uart_puts("---- in.patch ----\r\n");
        uart_puts((const char *)txt);
        uart_puts("------------------\r\n");
    }

    /* Teardown (models a reboot): unload the plugin; the DAC and input
     * singletons and the SD card persist. */
    pm_unload(&g_pm, (uint32_t)fp);
    g_n_io = 0;
    setup_input();
    int edges_after_teardown = count_edges();

    long ld = pm_patch_load(&g_pm, "/sd/in.patch");
    for (int j = 0; j < PM_MAX_PLUGINS; j++)
        if (g_pm.slots[j].used && !io_by_pid(g_pm.slots[j].pid))
            setup_plugin(g_pm.slots[j].pid);
    uint32_t fp2 = 0;
    for (int j = 0; j < PM_MAX_PLUGINS; j++)
        if (g_pm.slots[j].used) fp2 = g_pm.slots[j].pid;

    int graph_ok = (sv == PM_OK) && (ld == PM_OK) && has_input_line &&
                   (edges_after_teardown == 0) && (count_edges() == 2) &&
                   has_edge_pid(GRAPH_INPUT_PID, fp2) && has_edge_pid(fp2, 0u);
    uart_printf("C: save=%d load=%d input-line=%d edges=%d input->filter=%d filter->dac=%d\r\n",
                (int)sv, (int)ld, has_input_line, count_edges(),
                has_edge_pid(GRAPH_INPUT_PID, fp2), has_edge_pid(fp2, 0u));

    feed_capture(1);
    run_block();
    int reload_sound = page_has_sound(dac_input());

    int ok = chain_ok && exact && under_ok && graph_ok && reload_sound;
    uart_printf("checks: chain=%d bypass=%d underrun=%d patch=%d reload=%d\r\n",
                chain_ok, exact, under_ok, graph_ok, reload_sound);
    uart_puts(ok ? "GRAPH-INPUT: PASS\r\n" : "GRAPH-INPUT: FAIL\r\n");

    for (;;)
        __asm__ volatile("wfe");
}
