/* arch/arm64/shell_graph.c - graph commands for the serial shell (Issue #81, M13) */

#include "shell_graph.h"
#include "patch.h"          /* patch_parse_value: shared number->bits parsing */

static shell_graph_ops_t *ops_of(shell_t *sh)
{
    return (shell_graph_ops_t *)sh->ctx;
}

/* ---- small parsers / renderers (no libc, no printf) ------------------- */

int sg_parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s)
        return -1;
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        if (!s[2])
            return -1;
        for (int i = 2; s[i]; i++) {
            char c = s[i];
            uint32_t d;
            if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return -1;
            v = (v << 4) | d;
        }
    } else {
        for (int i = 0; s[i]; i++) {
            if (s[i] < '0' || s[i] > '9')
                return -1;
            v = v * 10u + (uint32_t)(s[i] - '0');
        }
    }
    *out = v;
    return 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void wr_u64(shell_t *sh, uint64_t v)
{
    char buf[20];
    int  n = 0;
    do { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    char out[21];
    int  k = 0;
    while (n) out[k++] = buf[--n];
    out[k] = '\0';
    shell_write(sh, out);
}

/* A `dst` token is either "dac" (the sink, pid 0) or a numeric pid. */
static int parse_dst(const char *s, uint32_t *out)
{
    if (streq(s, "dac")) { *out = 0; return 0; }
    return sg_parse_u32(s, out);
}

/* Render a failed action's error line and leave the graph untouched. */
static void fail(shell_t *sh, const char *verb, int code)
{
    shell_graph_ops_t *o = ops_of(sh);
    shell_write(sh, "error: ");
    shell_write(sh, verb);
    shell_write(sh, ": ");
    const char *msg = (o->strerror) ? o->strerror(o->be, verb, code) : 0;
    if (msg) {
        shell_write(sh, msg);
    } else {
        shell_write(sh, "code ");
        /* codes are negative; print with a leading '-' */
        if (code < 0) { shell_write(sh, "-"); wr_u64(sh, (uint64_t)(-code)); }
        else          { wr_u64(sh, (uint64_t)code); }
    }
    shell_write(sh, "\r\n");
}

static void usage(shell_t *sh, const char *u)
{
    shell_write(sh, "usage: ");
    shell_write(sh, u);
    shell_write(sh, "\r\n");
}

/* ---- commands --------------------------------------------------------- */

static int cmd_load(shell_t *sh, int argc, char **argv)
{
    if (argc != 2) { usage(sh, "load <path>"); return 0; }
    long pid = ops_of(sh)->load(ops_of(sh)->be, argv[1]);
    if (pid <= 0) { fail(sh, "load", (int)pid); return 0; }
    shell_write(sh, "loaded pid ");
    wr_u64(sh, (uint64_t)pid);
    shell_write(sh, "\r\n");
    return 0;
}

static int cmd_unload(shell_t *sh, int argc, char **argv)
{
    uint32_t pid;
    if (argc != 2 || sg_parse_u32(argv[1], &pid)) { usage(sh, "unload <pid>"); return 0; }
    int r = ops_of(sh)->unload(ops_of(sh)->be, pid);
    if (r < 0) { fail(sh, "unload", r); return 0; }
    shell_write(sh, "unloaded pid ");
    wr_u64(sh, pid);
    shell_write(sh, "\r\n");
    return 0;
}

static int cmd_wire(shell_t *sh, int argc, char **argv)
{
    uint32_t s, d;
    if (argc != 3 || sg_parse_u32(argv[1], &s) || parse_dst(argv[2], &d)) {
        usage(sh, "wire <src-pid> <dst-pid|dac>");
        return 0;
    }
    int r = ops_of(sh)->wire(ops_of(sh)->be, s, d);
    if (r < 0) { fail(sh, "wire", r); return 0; }
    shell_write(sh, "wired\r\n");
    return 0;
}

static int cmd_unwire(shell_t *sh, int argc, char **argv)
{
    uint32_t s, d;
    if (argc != 3 || sg_parse_u32(argv[1], &s) || parse_dst(argv[2], &d)) {
        usage(sh, "unwire <src-pid> <dst-pid|dac>");
        return 0;
    }
    int r = ops_of(sh)->unwire(ops_of(sh)->be, s, d);
    if (r < 0) { fail(sh, "unwire", r); return 0; }
    shell_write(sh, "unwired\r\n");
    return 0;
}

static int cmd_setparam(shell_t *sh, int argc, char **argv)
{
    uint32_t pid, param, bits;
    if (argc != 4 || sg_parse_u32(argv[1], &pid) || sg_parse_u32(argv[2], &param)) {
        usage(sh, "set-param <pid> <param-id> <value>");
        return 0;
    }
    if (patch_parse_value(argv[3], &bits) != PATCH_OK) {
        shell_write(sh, "error: set-param: bad value (decimal or 0xHEX)\r\n");
        return 0;
    }
    int r = ops_of(sh)->set_param(ops_of(sh)->be, pid, param, bits);
    if (r < 0) { fail(sh, "set-param", r); return 0; }
    shell_write(sh, "set\r\n");
    return 0;
}

static void wr_pid_or_dac(shell_t *sh, uint32_t pid)
{
    if (pid == 0) shell_write(sh, "dac");
    else          wr_u64(sh, pid);
}

static int cmd_ls(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    sg_view_t v;
    v.n_nodes = 0;
    v.n_edges = 0;
    ops_of(sh)->describe(ops_of(sh)->be, &v);

    shell_write(sh, "nodes:\r\n");
    if (v.n_nodes == 0)
        shell_write(sh, "  (none)\r\n");
    for (int i = 0; i < v.n_nodes; i++) {
        shell_write(sh, "  ");
        if (v.nodes[i].pid == 0) {
            shell_write(sh, "dac");
        } else {
            shell_write(sh, "pid ");
            wr_u64(sh, v.nodes[i].pid);
            if (v.nodes[i].name) {
                shell_write(sh, " ");
                shell_write(sh, v.nodes[i].name);
            }
            for (int p = 0; p < v.nodes[i].n_params; p++) {
                shell_write(sh, p == 0 ? "  params:" : "");
                shell_write(sh, " ");
                wr_u64(sh, v.nodes[i].params[p].id);
                shell_write(sh, "=");
                char hex[11];
                patch_format_value(v.nodes[i].params[p].bits, hex);
                shell_write(sh, hex);
            }
        }
        shell_write(sh, "\r\n");
    }

    shell_write(sh, "edges:\r\n");
    if (v.n_edges == 0)
        shell_write(sh, "  (none)\r\n");
    for (int i = 0; i < v.n_edges; i++) {
        shell_write(sh, "  ");
        wr_pid_or_dac(sh, v.edges[i].src);
        shell_write(sh, " -> ");
        wr_pid_or_dac(sh, v.edges[i].dst);
        shell_write(sh, "\r\n");
    }
    return 0;
}

static int cmd_stats(shell_t *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    sg_stats_t s;
    s.have_audio = 0;
    s.n_nodes    = 0;
    ops_of(sh)->get_stats(ops_of(sh)->be, &s);

    if (s.have_audio) {
        shell_write(sh, "audio: serviced=");
        wr_u64(sh, s.serviced);
        shell_write(sh, " overruns=");
        wr_u64(sh, s.overruns);
        shell_write(sh, "\r\n");
    }
    if (s.n_nodes == 0)
        shell_write(sh, "plugins: (none)\r\n");
    for (int i = 0; i < s.n_nodes; i++) {
        shell_write(sh, "plugin pid=");
        wr_u64(sh, s.nodes[i].pid);
        if (s.nodes[i].name) { shell_write(sh, " "); shell_write(sh, s.nodes[i].name); }
        shell_write(sh, " runs=");   wr_u64(sh, s.nodes[i].runs);
        shell_write(sh, " min=");    wr_u64(sh, s.nodes[i].min_us);
        shell_write(sh, "us max=");  wr_u64(sh, s.nodes[i].max_us);
        shell_write(sh, "us mean="); wr_u64(sh, s.nodes[i].mean_us);
        shell_write(sh, "us over="); wr_u64(sh, s.nodes[i].overruns);
        shell_write(sh, " off=");    wr_u64(sh, s.nodes[i].offences);
        shell_write(sh, "\r\n");
    }
    return 0;
}

static int cmd_patch(shell_t *sh, int argc, char **argv)
{
    shell_graph_ops_t *o = ops_of(sh);
    if (argc < 2) {
        usage(sh, "patch <save|load|ls> [path]");
        return 0;
    }

    if (streq(argv[1], "save")) {
        if (argc != 3 || !o->patch_save) { usage(sh, "patch save <path>"); return 0; }
        long r = o->patch_save(o->be, argv[2]);
        if (r < 0) { fail(sh, "patch save", (int)r); return 0; }
        shell_write(sh, "saved ");
        shell_write(sh, argv[2]);
        shell_write(sh, "\r\n");
        return 0;
    }
    if (streq(argv[1], "load")) {
        if (argc != 3 || !o->patch_load) { usage(sh, "patch load <path>"); return 0; }
        long r = o->patch_load(o->be, argv[2]);
        if (r < 0) { fail(sh, "patch load", (int)r); return 0; }
        shell_write(sh, "loaded ");
        shell_write(sh, argv[2]);
        shell_write(sh, "\r\n");
        return 0;
    }
    if (streq(argv[1], "ls")) {
        if (!o->patch_list) { shell_write(sh, "patches: (unsupported)\r\n"); return 0; }
        const char *names[SG_MAX_FILES];
        int n = o->patch_list(o->be, names, SG_MAX_FILES);
        shell_write(sh, "patches:\r\n");
        if (n <= 0)
            shell_write(sh, "  (none)\r\n");
        for (int i = 0; i < n && i < SG_MAX_FILES; i++) {
            shell_write(sh, "  ");
            shell_write(sh, names[i]);
            shell_write(sh, "\r\n");
        }
        return 0;
    }

    usage(sh, "patch <save|load|ls> [path]");
    return 0;
}

const shell_cmd_t shell_graph_cmds[] = {
    { "load",      "load a plugin: load <path>",           cmd_load     },
    { "unload",    "unload a plugin: unload <pid>",        cmd_unload   },
    { "wire",      "connect: wire <src> <dst|dac>",        cmd_wire     },
    { "unwire",    "disconnect: unwire <src> <dst|dac>",   cmd_unwire   },
    { "set-param", "set-param <pid> <id> <value>",         cmd_setparam },
    { "ls",        "list the graph",                       cmd_ls       },
    { "stats",     "show audio / per-plugin stats",        cmd_stats    },
    { "patch",     "patch save|load|ls <path>",            cmd_patch    },
};
const int shell_graph_ncmds = (int)(sizeof(shell_graph_cmds) /
                                    sizeof(shell_graph_cmds[0]));

void shell_graph_install(shell_t *sh, const shell_graph_ops_t *ops)
{
    sh->cmds   = shell_graph_cmds;
    sh->n_cmds = shell_graph_ncmds;
    sh->ctx    = (void *)ops;
}
