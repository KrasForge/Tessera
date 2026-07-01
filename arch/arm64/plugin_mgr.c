/* arch/arm64/plugin_mgr.c - plugin lifecycle manager (Issue #30, M7) */

#include "plugin_mgr.h"
#include "plugin_loader.h"
#include "param_queue.h"
#include "graph_control.h"
#include "elf64.h"
#include "vfs.h"
#include "process.h"
#include "vmem.h"
#include "pmm.h"
#include "pmem.h"
#include "plugin_abi.h"
#include <stdint.h>
#include <stddef.h>

/* Scratch buffer for reading an ELF off the SD card before it is mapped: a
 * generous upper bound on plugin image size (256 KiB == 64 pages). */
#define PM_SCRATCH_PAGES 64

void pm_init(plugin_mgr_t *m, graph_control_t *gc)
{
    for (int i = 0; i < PM_MAX_PLUGINS; i++) {
        m->slots[i].used = 0;
        m->slots[i].pid  = 0;
        m->slots[i].pq   = (param_queue_t *)0;
    }
    vfs_init(&m->vfs);
    m->gc = gc;
}

int pm_register_blob(plugin_mgr_t *m, const char *name, void *blob, size_t len)
{
    return vfs_add_ramdisk(&m->vfs, name, (const uint8_t *)blob, (uint32_t)len)
               ? PM_ENOMEM : PM_OK;
}

void pm_mount_sd(plugin_mgr_t *m, fat_fs_t *fat)
{
    vfs_mount_sd(&m->vfs, fat);
}

static pm_slot_t *slot_by_pid(plugin_mgr_t *m, uint32_t pid)
{
    for (int i = 0; i < PM_MAX_PLUGINS; i++)
        if (m->slots[i].used && m->slots[i].pid == pid)
            return &m->slots[i];
    return (pm_slot_t *)0;
}

long pm_load(plugin_mgr_t *m, const char *path)
{
    pm_slot_t *s = (pm_slot_t *)0;
    for (int i = 0; i < PM_MAX_PLUGINS; i++)
        if (!m->slots[i].used) { s = &m->slots[i]; break; }
    if (!s)
        return PM_ENOMEM;

    /* Resolve the path to ELF bytes.  Ramdisk files resolve zero-copy; an SD
     * file is read into a scratch buffer that we free once plugin_load() has
     * copied the segments into the plugin's own pages. */
    uintptr_t scratch_pa = 0;
    uint8_t  *scratch    = (uint8_t *)0;
    if (path[0] == '/' && path[1] == 's' && path[2] == 'd' && path[3] == '/') {
        scratch_pa = phys_alloc_contig(PM_SCRATCH_PAGES);
        if (!scratch_pa)
            return PM_ENOMEM;
        scratch = (uint8_t *)P2V(scratch_pa);
    }

    const uint8_t *elf = (const uint8_t *)0;
    long n = vfs_resolve(&m->vfs, path, &elf, scratch,
                         PM_SCRATCH_PAGES * PAGE_SIZE);
    if (n < 0 || !elf) {
        if (scratch_pa) phys_free_contig(scratch_pa, PM_SCRATCH_PAGES);
        return PM_ENOENT;
    }

    /* Validate the image before it ever runs: a well-formed AArch64 ELF that
     * imports nothing outside the plugin ABI (a self-contained plugin imports
     * nothing at all -> allowed list is empty). */
    if (elf64_validate(elf, (size_t)n) != 0) {
        if (scratch_pa) phys_free_contig(scratch_pa, PM_SCRATCH_PAGES);
        return PM_EBADELF;
    }
    if (elf64_disallowed_imports(elf, (size_t)n, (const char *const *)0, 0) != 0) {
        if (scratch_pa) phys_free_contig(scratch_pa, PM_SCRATCH_PAGES);
        return PM_EIMPORT;
    }

    int lr = plugin_load(&s->plugin, elf, (size_t)n, path);

    /* The plugin owns its pages now; the scratch read buffer is done with. */
    if (scratch_pa) phys_free_contig(scratch_pa, PM_SCRATCH_PAGES);

    if (lr != PLUGIN_OK)
        return PM_EBADELF;

    /* Sandbox the plugin: the only SVC the kernel will honour from it is the
     * controlled exit issued by its entry trampoline; any syscall from the
     * plugin's own body is fatal (issue #35). */
    process_set_svc_gate(s->plugin.proc, PLUGIN_TRAMP_VA);

    /* ABI handshake at EL0: the plugin must report a matching major version
     * before plugin_init() is ever entered.  A faulting / missing handshake
     * (-1) or a mismatched major rejects the load and tears the process down. */
    long ver = plugin_call_abi_version(&s->plugin);
    if (ver < 0 ||
        ((uint32_t)ver >> 16) != TESSERA_PLUGIN_ABI_VERSION_MAJOR) {
        process_destroy(s->plugin.proc);
        return PM_EABI;
    }

    /* Per-plugin parameter queue, mapped into the plugin at PARAM_Q_VA. */
    size_t pqpages = (pq_bytes(PM_PARAM_CAP) + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t pqpa = phys_alloc_contig(pqpages);
    if (!pqpa) {
        process_destroy(s->plugin.proc);
        return PM_ENOMEM;
    }
    s->pq = (param_queue_t *)P2V(pqpa);
    pq_init(s->pq, PM_PARAM_CAP);
    plugin_map_region(&s->plugin, PARAM_Q_VA, pqpa, pqpages * PAGE_SIZE,
                      VMM_READ | VMM_WRITE);

    s->used = 1;
    s->pid  = s->plugin.proc->pid;

    /* Remember the source path and reset the param record, for patch save. */
    int k = 0;
    for (; path[k] && k < PATCH_PATH_MAX - 1; k++) s->path[k] = path[k];
    s->path[k] = '\0';
    s->n_params = 0;

    if (m->gc)
        gc_add_plugin(m->gc, s->pid);

    return (long)s->pid;
}

int pm_unload(plugin_mgr_t *m, uint32_t pid)
{
    pm_slot_t *s = slot_by_pid(m, pid);
    if (!s)
        return PM_ENOENT;

    /* Disconnect every edge that touches this plugin first. */
    if (m->gc) {
        int self = audio_graph_node_by_pid(&m->gc->graph, pid);
        if (self >= 0) {
            for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
                if (!m->gc->graph.edges[e].used)
                    continue;
                if (m->gc->graph.edges[e].src == self ||
                    m->gc->graph.edges[e].dst == self) {
                    uint32_t sp = m->gc->graph.nodes[m->gc->graph.edges[e].src].pid;
                    uint32_t dp = m->gc->graph.nodes[m->gc->graph.edges[e].dst].pid;
                    pm_disconnect(m, sp, dp);
                }
            }
            audio_graph_remove_node(&m->gc->graph, self);
        }
    }

    /* process_destroy frees every user page (segments, stack, trampoline,
     * param page, and the mapped parameter queue) plus the page tables, the
     * L0 root, and the ASID - so nothing leaks. */
    process_destroy(s->plugin.proc);

    s->used = 0;
    s->pid  = 0;
    s->pq   = (param_queue_t *)0;
    return PM_OK;
}

int pm_set_param(plugin_mgr_t *m, uint32_t pid, uint32_t param_id, uint32_t value_bits)
{
    pm_slot_t *s = slot_by_pid(m, pid);
    if (!s)
        return PM_ENOENT;

    /* Remember the latest value per param id so a patch save can serialise it
     * (update in place if the id was set before, else append). */
    int found = 0;
    for (int i = 0; i < s->n_params; i++)
        if (s->params[i].id == param_id) { s->params[i].bits = value_bits; found = 1; break; }
    if (!found && s->n_params < PM_SLOT_PARAMS) {
        s->params[s->n_params].id   = param_id;
        s->params[s->n_params].bits = value_bits;
        s->n_params++;
    }

    return pq_push(s->pq, param_id, value_bits) ? PM_OK : PM_EFULL;
}

int pm_connect(plugin_mgr_t *m, uint32_t src_pid, uint32_t dst_pid)
{
    if (!m->gc)
        return PM_ENOENT;
    return gc_connect(m->gc, src_pid, dst_pid);
}

int pm_disconnect(plugin_mgr_t *m, uint32_t src_pid, uint32_t dst_pid)
{
    if (!m->gc)
        return PM_ENOENT;
    return gc_disconnect(m->gc, src_pid, dst_pid);
}

plugin_t *pm_plugin(plugin_mgr_t *m, uint32_t pid)
{
    pm_slot_t *s = slot_by_pid(m, pid);
    return s ? &s->plugin : (plugin_t *)0;
}

/* ---- patch / preset persistence (issue #40) ------------------------------ */

/* Patch index (position among used slots) for a live PID, or -1. */
static int patch_index_of_pid(plugin_mgr_t *m, uint32_t pid)
{
    int idx = 0;
    for (int j = 0; j < PM_MAX_PLUGINS; j++) {
        if (!m->slots[j].used) continue;
        if (m->slots[j].pid == pid) return idx;
        idx++;
    }
    return -1;
}

int pm_capture_patch(plugin_mgr_t *m, patch_t *p)
{
    patch_init(p);

    /* Plugins, in slot order. */
    for (int j = 0; j < PM_MAX_PLUGINS; j++)
        if (m->slots[j].used)
            if (patch_add_plugin(p, m->slots[j].path) < 0)
                return PM_ENOMEM;

    /* Remembered parameter values, per plugin. */
    int idx = 0;
    for (int j = 0; j < PM_MAX_PLUGINS; j++) {
        if (!m->slots[j].used) continue;
        for (int k = 0; k < m->slots[j].n_params; k++)
            patch_add_param(p, idx, m->slots[j].params[k].id,
                            m->slots[j].params[k].bits);
        idx++;
    }

    /* Graph wiring (a "dac" target, pid 0, becomes PATCH_DAC). */
    if (m->gc) {
        for (int e = 0; e < GRAPH_MAX_EDGES; e++) {
            if (!m->gc->graph.edges[e].used) continue;
            uint32_t sp = m->gc->graph.nodes[m->gc->graph.edges[e].src].pid;
            uint32_t dp = m->gc->graph.nodes[m->gc->graph.edges[e].dst].pid;
            int si = patch_index_of_pid(m, sp);
            int di = (dp == 0u) ? PATCH_DAC : patch_index_of_pid(m, dp);
            if (si >= 0 && (di == PATCH_DAC || di >= 0))
                patch_add_edge(p, si, di);
        }
    }
    return PM_OK;
}

int pm_apply_patch(plugin_mgr_t *m, const patch_t *p)
{
    uint32_t pid_of[PATCH_MAX_PLUGINS];

    for (int i = 0; i < p->n_plugins; i++) {
        long pid = pm_load(m, p->plugins[i].path);
        if (pid <= 0)
            return (int)pid;                 /* propagate the load error */
        pid_of[i] = (uint32_t)pid;
    }

    for (int i = 0; i < p->n_params; i++) {
        int pl = p->params[i].plugin;
        if (pl < 0 || pl >= p->n_plugins)
            return PM_ENOENT;
        pm_set_param(m, pid_of[pl], p->params[i].id, p->params[i].bits);
    }

    for (int i = 0; i < p->n_edges; i++) {
        int si = p->edges[i].src, di = p->edges[i].dst;
        if (si < 0 || si >= p->n_plugins) return PM_ENOENT;
        if (di != PATCH_DAC && (di < 0 || di >= p->n_plugins)) return PM_ENOENT;
        uint32_t sp = pid_of[si];
        uint32_t dp = (di == PATCH_DAC) ? 0u : pid_of[di];
        pm_connect(m, sp, dp);
    }
    return PM_OK;
}

/* Fixed temp path (in the target's backend) for atomic save. */
static const char *pm_tmp_path(const char *path)
{
    if (path[0] == '/' && path[1] == 's' && path[2] == 'd' && path[3] == '/')
        return "/sd/psave.tmp";
    if (path[0] == '/' && path[1] == 'r' && path[2] == 'd' && path[3] == '/')
        return "/rd/psave.tmp";
    return "psave.tmp";
}

long pm_patch_save(plugin_mgr_t *m, const char *path)
{
    static char buf[VFS_STORE_CAP];
    patch_t p;
    pm_capture_patch(m, &p);

    long n = patch_serialize(&p, buf, sizeof(buf));
    if (n < 0)
        return PM_ENOMEM;

    /* Atomic: write a temp file, then rename it over the target, so a crash
     * mid-write cannot corrupt the previous patch. */
    const char *tmp = pm_tmp_path(path);
    if (vfs_write(&m->vfs, tmp, (const uint8_t *)buf, (uint32_t)n) != VFS_OK)
        return PM_ENOMEM;
    if (vfs_rename(&m->vfs, tmp, path) != VFS_OK)
        return PM_ENOMEM;
    return PM_OK;
}

long pm_patch_load(plugin_mgr_t *m, const char *path)
{
    static uint8_t scratch[VFS_STORE_CAP];
    const uint8_t *data = (const uint8_t *)0;

    long n = vfs_resolve(&m->vfs, path, &data, scratch, sizeof(scratch));
    if (n < 0 || !data)
        return PM_ENOENT;

    patch_t p;
    if (patch_parse((const char *)data, (uint32_t)n, &p) != PATCH_OK)
        return PM_EBADELF;                   /* corrupt / truncated */

    return pm_apply_patch(m, &p);
}
