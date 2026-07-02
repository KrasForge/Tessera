/* tests/arm64/virt/shell_patch_main.c - patch persistence from the serial
 * console (Issue #82).
 *
 * The M13 "no C" acceptance: a user at the serial console builds a graph,
 * saves it, "reboots", reloads the patch, and the audio is identical - all
 * without compiling anything.
 *
 * A single core (MMU on) reads a scripted console session over `-serial
 * stdio` and drives the shell (issue #80) with the graph and patch commands
 * (issues #81/#82) bound to the real plugin manager, graph control plane, and
 * a writable in-RAM FAT16 "SD card".  The script:
 *
 *     load /rd/synth        (SDK sine generator)
 *     load /rd/effect       (state-variable low-pass)
 *     wire 1 2 ; wire 2 dac
 *     set-param 1 0 880     (tune the synth to 880 Hz)
 *     run                   (render one block -> reference audio hash)
 *     patch save /sd/live.patch
 *     reboot                (unload everything; the SD card survives)
 *     patch ls              (the saved patch is still on the card)
 *     patch load /sd/live.patch
 *     run                   (render again -> reloaded audio hash)
 *     patch load /sd/missing.patch   (error surfaces, kernel keeps running)
 *     done
 *
 * The harness asserts the reloaded audio hash equals the pre-reboot one (the
 * graph, its wiring, and the 880 Hz parameter all round-tripped through the
 * text patch on the SD card), that the missing-patch load was rejected without
 * a panic, and then powers off via PSCI so the log flushes.
 *
 * The "reboot" is modelled as in the M9 patch harness: the live graph is torn
 * down while the FAT SD image (a RAM buffer) and the plugin registrations
 * persist - exactly what a real reboot would keep on the card.
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
#include "fat.h"
#include "shell.h"
#include "shell_graph.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
void exceptions_init(void);

extern char synth_elf_start[],  synth_elf_end[];
extern char effect_elf_start[], effect_elf_end[];

static void psci_system_off(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000008u;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

/* ---- in-RAM writable FAT16 "SD card" (survives the reboot) ---- */
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

/* ---- ring backend: a real page so gc_connect records the edge ---- */
static void *ring_new(void *c)
{ (void)c; uintptr_t pa = phys_alloc_page_zero(); return pa ? P2V(pa) : (void *)0; }
static void  ring_del(void *c, void *r)                     { (void)c; (void)r; }
static int   ring_map(void *c, uint32_t p, void *r, int i)  { (void)c;(void)p;(void)r;(void)i; return 0; }
static void  ring_unmap(void *c, uint32_t p, void *r, int i){ (void)c;(void)p;(void)r;(void)i; }

static graph_control_t g_gc;
static plugin_mgr_t    g_pm;
static fat_fs_t        g_fat;

/* ---- per-plugin I/O pages (kernel-visible) ---- */
#define OUT_L_VA  RESULTS_VA
#define OUT_R_VA  (RESULTS_VA + (uint64_t)RING_BLOCK * 4u)
#define IN_L_VA   RING_IN_VA
#define IN_R_VA   (RING_IN_VA + (uint64_t)RING_BLOCK * 4u)

typedef struct { uint32_t pid; plugin_t *pl; float *out_k; float *in_k; uint64_t runs; } io_t;
static io_t g_io[GRAPH_MAX_NODES];
static int  g_n_io;

static io_t *io_by_pid(uint32_t pid)
{
    for (int i = 0; i < g_n_io; i++)
        if (g_io[i].pid == pid) return &g_io[i];
    return (io_t *)0;
}

static void setup_plugin(uint32_t pid)
{
    plugin_t *pl = pm_plugin(&g_pm, pid);
    if (!pl)
        return;
    io_t *e = &g_io[g_n_io++];
    e->pid = pid; e->pl = pl; e->runs = 0;
    uintptr_t out_pa = phys_alloc_page_zero();
    uintptr_t in_pa  = phys_alloc_page_zero();
    plugin_map_region(pl, RESULTS_VA, out_pa, PAGE_SIZE, VMM_READ | VMM_WRITE);
    plugin_map_region(pl, RING_IN_VA, in_pa,  PAGE_SIZE, VMM_READ | VMM_WRITE);
    e->out_k = (float *)P2V(out_pa);
    e->in_k  = (float *)P2V(in_pa);
    plugin_call_init(pl, RING_SR, RING_BLOCK);
}

static void page_clear(float *p) { uint32_t *w = (uint32_t *)p; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) w[i] = 0; }
static void page_copy(float *d, const float *s) { const uint32_t *sw = (const uint32_t *)s; uint32_t *dw = (uint32_t *)d; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) dw[i] = sw[i]; }
static uint32_t page_hash(const float *p) { const uint32_t *w = (const uint32_t *)p; uint32_t h = 2166136261u; for (uint32_t i = 0; i < RING_BLOCK * 2u; i++) h = (h ^ w[i]) * 16777619u; return h; }

/* ---- backend vtable (wraps pm_* / graph_control / FAT) ---- */
static uint32_t g_loaded;

static long b_load(void *be, const char *path)
{
    (void)be;
    long pid = pm_load(&g_pm, path);
    if (pid <= 0)
        return pid;
    setup_plugin((uint32_t)pid);
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
        if (g_gc.graph.nodes[i].type == NODE_UNUSED) continue;
        if (v->n_nodes >= SG_MAX_NODES) break;
        sg_node_t *n = &v->nodes[v->n_nodes++];
        n->pid = g_gc.graph.nodes[i].pid;
        n->name = 0; n->n_params = 0;
        for (int j = 0; j < PM_MAX_PLUGINS; j++)
            if (g_pm.slots[j].used && g_pm.slots[j].pid == n->pid)
                n->name = g_pm.slots[j].path;
    }
    v->n_edges = 0;
    for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
        if (!g_gc.graph.edges[e].used) continue;
        if (v->n_edges >= SG_MAX_EDGES) break;
        v->edges[v->n_edges].src = g_gc.graph.nodes[g_gc.graph.edges[e].src].pid;
        v->edges[v->n_edges].dst = g_gc.graph.nodes[g_gc.graph.edges[e].dst].pid;
        v->n_edges++;
    }
}

static void b_get_stats(void *be, sg_stats_t *s) { (void)be; s->have_audio = 0; s->n_nodes = 0; }

static long b_patch_save(void *be, const char *path) { (void)be; return pm_patch_save(&g_pm, path); }

static long b_patch_load(void *be, const char *path)
{
    (void)be;
    long r = pm_patch_load(&g_pm, path);
    if (r < 0)
        return r;
    /* pm_apply_patch loaded plugins via pm_load (no I/O map, no init); set up
     * every live plugin we are not already tracking. */
    for (int j = 0; j < PM_MAX_PLUGINS; j++)
        if (g_pm.slots[j].used && !io_by_pid(g_pm.slots[j].pid))
            setup_plugin(g_pm.slots[j].pid);
    return r;
}

/* fat_list collects names into this scratch, exposed as the shell's array. */
static char g_names[SG_MAX_FILES][13];
static const char **g_names_out;
static int g_nnames, g_names_max;
static void list_cb(void *ctx, const char *name)
{
    (void)ctx;
    if (g_nnames >= g_names_max || g_nnames >= SG_MAX_FILES)
        return;
    int k = 0;
    for (; name[k] && k < 12; k++) g_names[g_nnames][k] = name[k];
    g_names[g_nnames][k] = '\0';
    g_names_out[g_nnames] = g_names[g_nnames];
    g_nnames++;
}
static int b_patch_list(void *be, const char **names, int max)
{
    (void)be;
    g_nnames = 0; g_names_out = names; g_names_max = max;
    int r = fat_list(&g_fat, list_cb, 0);
    return (r < 0) ? r : g_nnames;
}

static int verb_is(const char *v, const char *n) { while (*v && *v == *n) { v++; n++; } return *v == *n; }
static const char *b_strerror(void *be, const char *verb, int code)
{
    (void)be;
    if (verb_is(verb, "wire") || verb_is(verb, "unwire")) {
        switch (code) {
        case GC_ENODEV: return "no such node";
        case GC_EEXIST: return "edge already exists";
        case GC_ENOENT: return "no such edge";
        default:        return 0;
        }
    }
    switch (code) {
    case PM_ENOENT:  return "no such file/pid";
    case PM_ENOMEM:  return "out of memory/slots";
    case PM_EBADELF: return "corrupt or invalid file";
    case PM_EABI:    return "ABI version mismatch";
    default:         return 0;
    }
}

static shell_graph_ops_t g_ops = {
    b_load, b_unload, b_wire, b_unwire, b_setparam,
    b_describe, b_get_stats,
    b_patch_save, b_patch_load, b_patch_list,
    b_strerror, 0
};

/* ---- harness commands: run, reboot, done ---- */
static uint32_t g_last_hash, g_ref_hash, g_runs;
static volatile int g_done;

static void run_one_block(void)
{
    int order[GRAPH_MAX_NODES];
    int n = audio_graph_toposort(&g_gc.graph, order, GRAPH_MAX_NODES);
    if (n < 0)
        return;
    float *dac_in = 0;
    for (int i = 0; i < n; i++) {
        int idx = order[i];
        if (g_gc.graph.nodes[idx].type == NODE_DAC) {
            for (int e = 0; e < GRAPH_MAX_EDGES; e++)
                if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                    io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                    if (u) dac_in = u->out_k;
                }
            continue;
        }
        io_t *me = io_by_pid(g_gc.graph.nodes[idx].pid);
        if (!me) continue;
        page_clear(me->in_k);
        for (int e = 0; e < GRAPH_MAX_EDGES; e++)
            if (g_gc.graph.edges[e].used && g_gc.graph.edges[e].dst == idx) {
                io_t *u = io_by_pid(g_gc.graph.nodes[g_gc.graph.edges[e].src].pid);
                if (u) page_copy(me->in_k, u->out_k);
            }
        plugin_call_block(me->pl, IN_L_VA, IN_R_VA, OUT_L_VA, OUT_R_VA, RING_BLOCK);
        me->runs++;
    }
    g_last_hash = dac_in ? page_hash(dac_in) : 0;
    g_runs++;
}

static void wr_hex(shell_t *sh, uint32_t v)
{
    static const char h[] = "0123456789abcdef";
    char out[11];
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = h[(v >> ((7 - i) * 4)) & 0xF];
    out[10] = '\0';
    shell_write(sh, out);
}

static int h_run(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    run_one_block();
    shell_write(sh, "rendered one block; dac-hash ");
    wr_hex(sh, g_last_hash);
    shell_write(sh, "\r\n");
    return 0;
}

static int h_reboot(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Snapshot the pre-reboot audio, then tear the live graph down.  The FAT
     * SD image and the plugin registrations persist, as they would on a real
     * reboot from the card. */
    g_ref_hash = g_last_hash;
    uint32_t pids[PM_MAX_PLUGINS];
    int np = 0;
    for (int j = 0; j < PM_MAX_PLUGINS; j++)
        if (g_pm.slots[j].used) pids[np++] = g_pm.slots[j].pid;
    for (int i = 0; i < np; i++)
        pm_unload(&g_pm, pids[i]);
    g_n_io = 0;
    shell_write(sh, "rebooted (graph cleared; SD card intact)\r\n");
    return 0;
}

static int h_done(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_write(sh, "bye\r\n");
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static shell_cmd_t g_cmds[SG_MAX_NODES + 4];
static int g_ncmds;

static void sh_out(void *io, const char *s) { (void)io; uart_puts(s); }

void test_main(void)
{
    uart_virt_init();
    uart_puts("\r\n=== QEMU virt shell patch persistence (issue #82) ===\r\n");

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
    pm_register_blob(&g_pm, "synth",  synth_elf_start,  (size_t)(synth_elf_end  - synth_elf_start));
    pm_register_blob(&g_pm, "effect", effect_elf_start, (size_t)(effect_elf_end - effect_elf_start));

    for (int i = 0; i < shell_graph_ncmds; i++)
        g_cmds[g_ncmds++] = shell_graph_cmds[i];
    g_cmds[g_ncmds++] = (shell_cmd_t){ "run",    "render one audio block", h_run    };
    g_cmds[g_ncmds++] = (shell_cmd_t){ "reboot", "tear down; keep the SD", h_reboot };
    g_cmds[g_ncmds++] = (shell_cmd_t){ "done",   "finish the demo",        h_done   };

    shell_t sh;
    shell_init(&sh, g_cmds, g_ncmds, sh_out, 0);
    sh.ctx    = &g_ops;
    sh.prompt = "tessera> ";
    shell_prompt(&sh);

    while (!__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        int c = uart_try_getc();
        if (c < 0) { __asm__ volatile("yield"); continue; }
        shell_feed(&sh, (char)c);
    }

    int identical = (g_ref_hash != 0) && (g_ref_hash == g_last_hash);
    uart_printf("summary: loaded=%u runs=%u ref-hash=0x%x reload-hash=0x%x identical=%d\r\n",
                (unsigned)g_loaded, (unsigned)g_runs,
                (unsigned)g_ref_hash, (unsigned)g_last_hash, identical);
    uart_printf("checks: identical=%d done=%d\r\n",
                identical, __atomic_load_n(&g_done, __ATOMIC_ACQUIRE));

    int ok = identical && __atomic_load_n(&g_done, __ATOMIC_ACQUIRE);
    uart_puts(ok ? "SHELL-PATCH: PASS\r\n" : "SHELL-PATCH: FAIL\r\n");

    psci_system_off();
    for (;;)
        __asm__ volatile("wfe");
}
