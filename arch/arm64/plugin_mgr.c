/* arch/arm64/plugin_mgr.c - plugin lifecycle manager (Issue #30, M7) */

#include "plugin_mgr.h"
#include "plugin_loader.h"
#include "param_queue.h"
#include "graph_control.h"
#include "process.h"
#include "vmem.h"
#include "pmm.h"
#include "pmem.h"
#include <stdint.h>
#include <stddef.h>

/* Bounded string compare (no libc in the kernel). */
static int streq(const char *a, const char *b)
{
    for (int i = 0; i < 64; i++) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == '\0')
            return 1;
    }
    return 0;
}

void pm_init(plugin_mgr_t *m, graph_control_t *gc)
{
    for (int i = 0; i < PM_MAX_PLUGINS; i++) {
        m->slots[i].used = 0;
        m->slots[i].pid  = 0;
        m->slots[i].pq   = (param_queue_t *)0;
    }
    m->n_blobs = 0;
    m->gc      = gc;
}

int pm_register_blob(plugin_mgr_t *m, const char *name, void *blob, size_t len)
{
    if (m->n_blobs >= PM_MAX_PLUGINS)
        return PM_ENOMEM;
    m->blobs[m->n_blobs].name = name;
    m->blobs[m->n_blobs].blob = blob;
    m->blobs[m->n_blobs].len  = len;
    m->n_blobs++;
    return PM_OK;
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
    const pm_blob_t *b = (const pm_blob_t *)0;
    for (int i = 0; i < m->n_blobs; i++)
        if (streq(m->blobs[i].name, path)) { b = &m->blobs[i]; break; }
    if (!b)
        return PM_ENOENT;

    pm_slot_t *s = (pm_slot_t *)0;
    for (int i = 0; i < PM_MAX_PLUGINS; i++)
        if (!m->slots[i].used) { s = &m->slots[i]; break; }
    if (!s)
        return PM_ENOMEM;

    if (plugin_load(&s->plugin, b->blob, b->len, b->name) != PLUGIN_OK)
        return PM_EBADELF;

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
