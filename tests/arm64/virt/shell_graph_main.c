/* tests/arm64/virt/shell_graph_main.c - building an audio graph from the
 * serial console (Issue #81).
 *
 * The M13 acceptance for the graph commands: starting from an empty graph on
 * a running system, a user at the serial console builds a synth -> filter ->
 * DAC chain and hears the result, without compiling any C.
 *
 * A single core (MMU on) reads a scripted console session over `-serial
 * stdio` and drives the shell (issue #80) with the graph commands (issue
 * #81) bound to a backend that wraps the real plugin manager and graph
 * control plane.  The script:
 *
 *     ls                     (empty graph)
 *     load /rd/synth         -> pid 1   (a sine generator)
 *     load /rd/effect        -> pid 2   (a low-pass filter)
 *     wire 1 2               (synth -> filter)
 *     wire 2 dac             (filter -> DAC)
 *     set-param 1 0 660      (tweak the synth live)
 *     ls                     (shows the chain)
 *     run 8                  (execute 8 audio blocks through the built graph)
 *     stats                  (audio + per-plugin run counts)
 *     done
 *
 * `run` walks the graph in the toposort order of the *actual* recorded edges,
 * running each plugin's process_block at EL0 and copying samples along each
 * edge, with the DAC reading the final stage; it asserts real (non-silent)
 * audio came out.  The harness then confirms the resulting graph (two plugin
 * nodes, two edges) and powers off via PSCI so the serial log flushes.
 *
 * Built MMU-on (virt_mmu.ld) with the virt GIC bases; run with -smp 1.
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
#include "audio_ringbuf.h"
#include "ring_contract.h"
#include "shell.h"
#include "shell_graph.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char synth_elf_start[], synth_elf_end[];
extern char effect_elf_start[], effect_elf_end[];

static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

/* ---- ring backend for the control plane (real rings, like graph_ctl) ---- */
static void *ring_new(void *ctx)
{
    (void)ctx;
    size_t pages = (arb_bytes(RING_FRAMES) + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t pa = phys_alloc_contig(pages);
    if (!pa)
        return (void *)0;
    audio_ring_hdr_t *h = (audio_ring_hdr_t *)P2V(pa);
    arb_init(h, RING_FRAMES);
    return h;
}
static void ring_del(void *ctx, void *r)              { (void)ctx; (void)r; }
static int  ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;

/* ---- per-plugin I/O pages (kernel-visible) ---- */
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

typedef struct { uint32_t pid; plugin_t *pl; float *out_k; float *in_k; uint64_t runs; } io_t;
static io_t   g_io[GRAPH_MAX_NODES];
static int    g_n_io;
static uint32_t g_loaded, g_run_blocks, g_run_sound;
static volatile int g_done;

static io_t *io_by_pid(uint32_t pid)
{
    for (int i = 0; i < g_n_io; i++)
        if (g_io[i].pid == pid)
            return &g_io[i];
    return (io_t *)0;
}

static void map_io(plugin_t *pl, float **out_k, float **in_k)
{
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ);
    *out_k = (float *)P2V(out_pa);
    *in_k  = (float *)P2V(in_pa);
}

static void page_clear(float *p) { uint32_t *w = (uint32_t *)p; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) w[i] = 0; }
static void page_copy(float *d, const float *s) { const uint32_t *sw = (const uint32_t *)s; uint32_t *dw = (uint32_t *)d; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) dw[i] = sw[i]; }
static int  page_has_sound(const float *p) { const uint32_t *w = (const uint32_t *)p; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) if (w[i]) return 1; return 0; }

/* ---- backend vtable (wraps pm_* / graph_control) ---- */
static long b_load(void *be, const char *path)
{
    (void)be;
    long pid = pm_load(&g_pm, path);
    if (pid <= 0)
        return pid;
    plugin_t *pl = pm_plugin(&g_pm, (uint32_t)pid);
    io_t *e = &g_io[g_n_io++];
    e->pid = (uint32_t)pid;
    e->pl  = pl;
    e->runs = 0;
    map_io(pl, &e->out_k, &e->in_k);
    if (plugin_call_init(pl, RING_SR, RING_BLOCK) != TESSERA_PLUGIN_OK)
        return -100;                       /* init faulted */
    g_loaded++;
    return pid;
}
static int b_unload(void *be, uint32_t pid) { (void)be; return pm_unload(&g_pm, pid); }
static int b_wire(void *be, uint32_t s, uint32_t d)   { (void)be; return pm_connect(&g_pm, s, d); }
static int b_unwire(void *be, uint32_t s, uint32_t d) { (void)be; return pm_disconnect(&g_pm, s, d); }
static int b_setparam(void *be, uint32_t pid, uint32_t pr, uint32_t bits)
{ (void)be; return pm_set_param(&g_pm, pid, pr, bits); }

static void b_describe(void *be, sg_view_t *v)
{
    (void)be;
    v->n_nodes = 0;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        if (g_gc.graph.nodes[i].type == NODE_UNUSED)
            continue;
        if (v->n_nodes >= SG_MAX_NODES)
            break;
        sg_node_t *n = &v->nodes[v->n_nodes++];
        n->pid = g_gc.graph.nodes[i].pid;
        n->name = 0;
        n->n_params = 0;
        for (int j = 0; j < PM_MAX_PLUGINS; j++)
            if (g_pm.slots[j].used && g_pm.slots[j].pid == n->pid) {
                n->name = g_pm.slots[j].path;
                for (int k = 0; k < g_pm.slots[j].n_params && k < SG_MAX_PARAMS; k++) {
                    n->params[n->n_params].id   = g_pm.slots[j].params[k].id;
                    n->params[n->n_params].bits = g_pm.slots[j].params[k].bits;
                    n->n_params++;
                }
            }
    }
    v->n_edges = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        if (!g_gc.graph.edges[e].used)
            continue;
        if (v->n_edges >= SG_MAX_EDGES)
            break;
        v->edges[v->n_edges].src = g_gc.graph.nodes[g_gc.graph.edges[e].src].pid;
        v->edges[v->n_edges].dst = g_gc.graph.nodes[g_gc.graph.edges[e].dst].pid;
        v->n_edges++;
    }
}

static void b_get_stats(void *be, sg_stats_t *s)
{
    (void)be;
    s->have_audio = 1;
    s->serviced   = g_run_blocks;
    s->overruns   = 0;
    s->n_nodes    = 0;
    for (int i = 0; i < g_n_io && s->n_nodes < SG_MAX_NODES; i++) {
        sg_stat_t *n = &s->nodes[s->n_nodes++];
        n->pid = g_io[i].pid;
        n->name = 0;
        n->runs = g_io[i].runs;
        n->min_us = n->max_us = n->mean_us = 0;   /* no clock in this harness */
        n->overruns = n->offences = 0;
    }
}

static int verb_is(const char *v, const char *name)
{
    while (*v && *v == *name) { v++; name++; }
    return *v == *name;
}

static const char *b_strerror(void *be, const char *verb, int code)
{
    (void)be;
    /* wire/unwire return graph-control codes; the rest return plugin-manager
     * codes.  The two enums overlap numerically, hence the verb split. */
    if (verb_is(verb, "wire") || verb_is(verb, "unwire")) {
        switch (code) {
        case GC_ENODEV: return "no such node";
        case GC_EEXIST: return "edge already exists";
        case GC_ENOENT: return "no such edge";
        case GC_ENOMEM: return "out of ring memory";
        case GC_EINVAL: return "invalid connection";
        default:        return 0;
        }
    }
    switch (code) {
    case PM_ENOENT:  return "no such plugin/pid";
    case PM_ENOMEM:  return "out of memory/slots";
    case PM_EBADELF: return "not a valid plugin ELF";
    case PM_EFULL:   return "parameter queue full";
    case PM_EABI:    return "ABI version mismatch";
    case PM_EIMPORT: return "disallowed imports";
    case -100:       return "plugin init faulted";
    default:         return 0;
    }
}

static shell_graph_ops_t g_ops = {
    b_load, b_unload, b_wire, b_unwire, b_setparam,
    b_describe, b_get_stats, b_strerror, 0
};

/* ---- harness commands: run, done ---- */
static void run_graph_block(void)
{
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g_gc.graph, order, GRAPH_MAX_NODES);
    if (n < 0)
        return;
    for (int i = 0; i < n; i++) {
        int idx = order[i];
        uint32_t pid = g_gc.graph.nodes[idx].pid;

        if (g_gc.graph.nodes[idx].type == NODE_DAC) {
            /* the DAC output is the upstream stage's output page */
            for (int e = 0; e < GRAPH_MAX_EDGES; e++)
                if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                    io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                    if (u && page_has_sound(u->out_k))
                        g_run_sound++;
                }
            continue;
        }

        io_t *me = io_by_pid(pid);
        if (!me)
            continue;
        page_clear(me->in_k);
        for (int e = 0; e < GRAPH_MAX_EDGES; e++)
            if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                if (u)
                    page_copy(me->in_k, u->out_k);      /* upstream -> my input */
            }
        plugin_call_block(me->pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
        me->runs++;
    }
    g_run_blocks++;
}

static int h_run(shell_t *sh, int argc, char **argv)
{
    uint32_t k = 8;
    if (argc >= 2 && sg_parse_u32(argv[1], &k) != 0) {
        shell_write(sh, "usage: run [blocks]\r\n");
        return 0;
    }
    uint32_t sound0 = g_run_sound;
    for (uint32_t b = 0; b < k; b++)
        run_graph_block();

    char buf[12];
    int  m = 0;
    uint32_t v = g_run_sound - sound0;
    do { buf[m++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    char num[13];
    int  j = 0;
    while (m) num[j++] = buf[--m];
    num[j] = '\0';
    shell_write(sh, "ran blocks; produced audio on ");
    shell_write(sh, num);
    shell_write(sh, " of them\r\n");
    return 0;
}

static int h_done(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_write(sh, "bye\r\n");
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return 0;
}

/* combined table: graph commands + harness commands */
static shell_cmd_t g_cmds[SG_MAX_NODES + 4];
static int g_ncmds;

static void sh_out(void *io, const char *s) { (void)io; uart_puts(s); }

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt shell graph commands (issue #81) ===\r\n");

    pmm_init();
    mmu_init();
    exceptions_init();

    gc_ring_ops_t ops = { ring_new, ring_del, ring_map, ring_unmap, 0 };
    gc_init(&g_gc, &ops);
    pm_init(&g_pm, &g_gc);
    gc_add_dac(&g_gc);              /* the sink exists from boot */
    pm_register_blob(&g_pm, "synth",  synth_elf_start,  (size_t)(synth_elf_end  - synth_elf_start));
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));

    /* Build the command table: graph commands, then run + done. */
    for (int i = 0; i < shell_graph_ncmds; i++)
        g_cmds[g_ncmds++] = shell_graph_cmds[i];
    g_cmds[g_ncmds++] = (shell_cmd_t){ "run",  "run <n> audio blocks", h_run  };
    g_cmds[g_ncmds++] = (shell_cmd_t){ "done", "finish the demo",      h_done };

    shell_t sh;
    shell_init(&sh, g_cmds, g_ncmds, sh_out, 0);
    sh.ctx    = &g_ops;
    sh.prompt = "tessera> ";
    shell_prompt(&sh);

    /* Single-core event loop: drain the console, dispatch commands. */
    while (!__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        int c = uart_try_getc();
        if (c < 0) { __asm__ volatile("yield"); continue; }
        shell_feed(&sh, (char)c);
    }

    /* ---- verify the resulting graph ---- */
    int plugin_nodes = 0, dac_nodes = 0, edges = 0;
    for (int i = 0; i < GRAPH_MAX_NODES; i++) {
        if (g_gc.graph.nodes[i].type == NODE_PLUGIN) plugin_nodes++;
        if (g_gc.graph.nodes[i].type == NODE_DAC)    dac_nodes++;
    }
    for (int e = 0; e < GRAPH_MAX_EDGES; e++)
        if (g_gc.graph.edges[e].used) edges++;

    uart_printf("summary: loaded=%u plugin-nodes=%d edges=%d run-blocks=%u run-sound=%u\r\n",
                (unsigned)g_loaded, plugin_nodes, edges,
                (unsigned)g_run_blocks, (unsigned)g_run_sound);

    int graph_ok = (g_loaded == 2) && (plugin_nodes == 2) &&
                   (dac_nodes == 1) && (edges == 2);
    int audio_ok = (g_run_blocks >= 8) && (g_run_sound == g_run_blocks);

    uart_printf("checks: graph=%d audio=%d done=%d\r\n",
                graph_ok, audio_ok, __atomic_load_n(&g_done, __ATOMIC_ACQUIRE));

    int ok = graph_ok && audio_ok && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE);
    uart_puts(ok ? "SHELL-GRAPH: PASS\r\n" : "SHELL-GRAPH: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
