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
