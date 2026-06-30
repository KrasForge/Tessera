/* arch/arm64/plugin_mgr.h - plugin lifecycle manager (Issue #30, M7)
 *
 * The kernel side of the control syscalls.  It keeps a table of loaded plugins
 * (pid -> process, parameter queue) and implements load / unload / set-param /
 * connect / disconnect.  Unload is leak-free: process_destroy() releases every
 * page the plugin owns (segments, stack, trampoline, param, and the mapped
 * parameter queue) along with its page tables and ASID, so loading and
 * unloading repeatedly returns the physical allocator to its baseline.
 *
 * Plugins are resolved by path through a tiny VFS (issue #34): "/sd/<name>"
 * comes off the FAT16 SD volume, "/rd/<name>" or a bare "<name>" comes from
 * the in-memory ramdisk.  Each image is validated (AArch64 ELF, no disallowed
 * imports, matching ABI major version via an EL0 handshake) before plugin_init
 * runs.  Graph wiring is delegated to the control plane from issue #28.
 */

#ifndef ARM64_PLUGIN_MGR_H
#define ARM64_PLUGIN_MGR_H

#include "plugin_loader.h"
#include "param_queue.h"
#include "graph_control.h"
#include "vfs.h"
#include <stdint.h>
#include <stddef.h>

#define PM_MAX_PLUGINS 16
#define PM_PARAM_CAP   16u           /* parameter-queue depth (events) */

/* User VA at which each plugin's parameter queue is mapped (see ring_contract). */
#ifndef PARAM_Q_VA
#define PARAM_Q_VA (0x8000000000ull + 0x0E000000ull)
#endif

typedef struct {
    int           used;
    uint32_t      pid;
    plugin_t      plugin;
    param_queue_t *pq;     /* kernel-visible alias of the param queue */
} pm_slot_t;

typedef struct {
    pm_slot_t       slots[PM_MAX_PLUGINS];
    vfs_t           vfs;   /* plugin sources: ramdisk + SD (issue #34) */
    graph_control_t *gc;   /* optional: graph wiring (issue #28)        */
} plugin_mgr_t;

/* Errors (negative). */
#define PM_OK        0
#define PM_ENOENT  (-1)   /* no such file / pid          */
#define PM_ENOMEM  (-2)   /* out of slots / memory       */
#define PM_EBADELF (-3)   /* not a valid AArch64 ELF     */
#define PM_EFULL   (-4)   /* parameter queue full        */
#define PM_EABI    (-5)   /* wrong plugin ABI version    */
#define PM_EIMPORT (-6)   /* disallowed import symbol(s) */

void pm_init(plugin_mgr_t *m, graph_control_t *gc);

/* Register a ramdisk plugin image under `name` (loadable as "<name>" or
 * "/rd/<name>"). */
int pm_register_blob(plugin_mgr_t *m, const char *name, void *blob, size_t len);

/* Mount the SD-card FAT volume (plugins loadable as "/sd/<name>"). */
void pm_mount_sd(plugin_mgr_t *m, fat_fs_t *fat);

/* sys_plugin_load: load `path` into a new isolated process with its own
 * parameter queue.  Returns the new PID (> 0) or a negative PM_E*. */
long pm_load(plugin_mgr_t *m, const char *path);

/* sys_plugin_unload: disconnect the plugin's edges, destroy the process, and
 * free every resource.  Returns PM_OK or a negative error. */
int pm_unload(plugin_mgr_t *m, uint32_t pid);

/* sys_plugin_set_param: enqueue (param_id, value-bits) for the plugin. */
int pm_set_param(plugin_mgr_t *m, uint32_t pid, uint32_t param_id, uint32_t value_bits);

/* sys_graph_connect / _disconnect (unified here): delegate to the graph. */
int pm_connect(plugin_mgr_t *m, uint32_t src_pid, uint32_t dst_pid);
int pm_disconnect(plugin_mgr_t *m, uint32_t src_pid, uint32_t dst_pid);

/* The loaded plugin for `pid` (for the harness to run it), or NULL. */
plugin_t *pm_plugin(plugin_mgr_t *m, uint32_t pid);

#endif /* ARM64_PLUGIN_MGR_H */
