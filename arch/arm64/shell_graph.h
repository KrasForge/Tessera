/* arch/arm64/shell_graph.h - graph commands for the serial shell (Issue #81, M13)
 *
 * The control-plane verbs of M7 (issue #30: load, unload, wire, unwire,
 * set-param) plus `ls` and `stats`, wired into the shell dispatcher of issue
 * #80 as ordinary command-table entries.  With them a user at the serial
 * console can build a synth -> filter -> DAC graph, tweak parameters live,
 * and inspect the running system - without compiling any C.
 *
 * The actions that touch the kernel-only plugin manager go through a vtable
 * (shell_graph_ops_t), exactly as graph_control injects its ring ops, so the
 * argument parsing, dispatch, error paths, and `ls`/`stats` rendering are all
 * pure C and host-tested (make test-arm-shell-graph) against a mock backend.
 * On the target the vtable wraps pm_* / graph_control / plugin_time; the
 * kernel structs never leak into the shell.
 */

#ifndef ARM64_SHELL_GRAPH_H
#define ARM64_SHELL_GRAPH_H

#include "shell.h"
#include <stdint.h>

#define SG_MAX_NODES  16
#define SG_MAX_EDGES  32
#define SG_MAX_PARAMS 8         /* params shown per node by `ls`            */
#define SG_MAX_FILES  16        /* patch files listed by `patch ls`         */

/* A graph node as `ls` sees it. */
typedef struct {
    uint32_t pid;              /* 0 marks the DAC sink                      */
    const char *name;          /* source path/name, or NULL                */
    int      n_params;
    struct { uint32_t id; uint32_t bits; } params[SG_MAX_PARAMS];
} sg_node_t;

typedef struct { uint32_t src; uint32_t dst; } sg_edge_t;   /* dst 0 = DAC  */

typedef struct {
    sg_node_t nodes[SG_MAX_NODES];
    int       n_nodes;
    sg_edge_t edges[SG_MAX_EDGES];
    int       n_edges;
} sg_view_t;

/* Per-plugin service-time stats as `stats` shows them (already in us). */
typedef struct {
    uint32_t pid;
    const char *name;
    uint64_t runs;
    uint64_t min_us, max_us, mean_us;
    uint64_t overruns;
    uint64_t offences;
} sg_stat_t;

typedef struct {
    int      have_audio;       /* 0 if the audio summary is unavailable    */
    uint32_t serviced;
    uint32_t overruns;
    sg_stat_t nodes[SG_MAX_NODES];
    int       n_nodes;
} sg_stats_t;

/* The backend the commands act through.  Every action returns 0 / a positive
 * pid on success or a negative error code; `strerror` (optional) turns a code
 * into a human string, else the shell prints the number. */
typedef struct shell_graph_ops {
    long (*load)(void *be, const char *path);            /* -> pid or <0    */
    int  (*unload)(void *be, uint32_t pid);
    int  (*wire)(void *be, uint32_t src, uint32_t dst);  /* dst 0 = DAC     */
    int  (*unwire)(void *be, uint32_t src, uint32_t dst);
    int  (*set_param)(void *be, uint32_t pid, uint32_t param, uint32_t bits);
    void (*describe)(void *be, sg_view_t *v);            /* fill for `ls`   */
    void (*get_stats)(void *be, sg_stats_t *s);          /* fill for `stats`*/
    /* Patch persistence (issue #82); any may be NULL if unsupported. */
    long (*patch_save)(void *be, const char *path);      /* -> 0 or <0      */
    long (*patch_load)(void *be, const char *path);      /* -> 0 or <0      */
    int  (*patch_list)(void *be, const char **names, int max);  /* -> count  */
    /* Optional: map a failing verb's code to a message.  Verb-aware because
     * the plugin-manager and graph-control error enums overlap numerically. */
    const char *(*strerror)(void *be, const char *verb, int code);
    void *be;                                            /* backend handle  */
} shell_graph_ops_t;

/* The graph command table (load/unload/wire/unwire/set-param/ls/stats).  The
 * caller points sh->ctx at a shell_graph_ops_t and includes these entries in
 * the table passed to shell_init. */
extern const shell_cmd_t shell_graph_cmds[];
extern const int         shell_graph_ncmds;

/* Convenience: install exactly the graph commands into `sh` with `ops` as the
 * backend (sets sh->cmds/n_cmds/ctx).  Use the table above directly when you
 * want to append your own commands. */
void shell_graph_install(shell_t *sh, const shell_graph_ops_t *ops);

/* Parse an unsigned 32-bit value: decimal, or 0x-prefixed hex.  Returns 0 on
 * success.  Exposed for testing. */
int sg_parse_u32(const char *s, uint32_t *out);

#endif /* ARM64_SHELL_GRAPH_H */
