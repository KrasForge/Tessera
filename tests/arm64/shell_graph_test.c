/* tests/arm64/shell_graph_test.c - host unit tests for the shell graph
 * commands (Issue #81).
 *
 * The commands act through a vtable, so the parsing, dispatch, error paths,
 * and ls/stats rendering are exercised on the host against a mock backend
 * that records calls and returns canned results - no MMU, no plugin loader.
 * This proves each verb parses its arguments, forwards the right values,
 * reports a one-line error (and touches nothing) on failure, and renders the
 * listing and stats as specified.
 *
 * Build/run via:  make test-arm-shell-graph
 */

#include "shell.h"
#include "shell_graph.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* ---- capture sink ---- */
static char g_out[8192];
static int  g_out_len;
static void cap_out(void *io, const char *s)
{
    (void)io;
    while (*s && g_out_len < (int)sizeof(g_out) - 1)
        g_out[g_out_len++] = *s++;
    g_out[g_out_len] = '\0';
}
static void cap_reset(void) { g_out_len = 0; g_out[0] = '\0'; }
static int  saw(const char *n) { return strstr(g_out, n) != 0; }

/* ---- mock backend ---- */
static struct {
    int  load_calls; char load_path[64];  long load_ret;
    int  unload_calls; uint32_t unload_pid; int unload_ret;
    int  wire_calls; uint32_t wire_s, wire_d; int wire_ret;
    int  unwire_calls; uint32_t unwire_s, unwire_d; int unwire_ret;
    int  sp_calls; uint32_t sp_pid, sp_param, sp_bits; int sp_ret;
} M;

static long m_load(void *be, const char *p)
{ (void)be; M.load_calls++; strncpy(M.load_path, p, 63); return M.load_ret; }
static int  m_unload(void *be, uint32_t pid)
{ (void)be; M.unload_calls++; M.unload_pid = pid; return M.unload_ret; }
static int  m_wire(void *be, uint32_t s, uint32_t d)
{ (void)be; M.wire_calls++; M.wire_s = s; M.wire_d = d; return M.wire_ret; }
static int  m_unwire(void *be, uint32_t s, uint32_t d)
{ (void)be; M.unwire_calls++; M.unwire_s = s; M.unwire_d = d; return M.unwire_ret; }
static int  m_setparam(void *be, uint32_t pid, uint32_t pr, uint32_t bits)
{ (void)be; M.sp_calls++; M.sp_pid = pid; M.sp_param = pr; M.sp_bits = bits; return M.sp_ret; }

static void m_describe(void *be, sg_view_t *v)
{
    (void)be;
    v->n_nodes = 3;
    v->nodes[0].pid = 2; v->nodes[0].name = "synth"; v->nodes[0].n_params = 1;
    v->nodes[0].params[0].id = 0; v->nodes[0].params[0].bits = 0x445c0000u;
    v->nodes[1].pid = 3; v->nodes[1].name = "filter"; v->nodes[1].n_params = 0;
    v->nodes[2].pid = 0; v->nodes[2].name = 0; v->nodes[2].n_params = 0;  /* dac */
    v->n_edges = 2;
    v->edges[0].src = 2; v->edges[0].dst = 3;
    v->edges[1].src = 3; v->edges[1].dst = 0;   /* -> dac */
}

static void m_stats(void *be, sg_stats_t *s)
{
    (void)be;
    s->have_audio = 1; s->serviced = 1000; s->overruns = 0;
    s->n_nodes = 1;
    s->nodes[0].pid = 2; s->nodes[0].name = "synth";
    s->nodes[0].runs = 500; s->nodes[0].min_us = 10; s->nodes[0].max_us = 15;
    s->nodes[0].mean_us = 12; s->nodes[0].overruns = 0; s->nodes[0].offences = 0;
}

static struct {
    int  save_calls; char save_path[64]; long save_ret;
    int  load_calls; char load_pth[64];  long load_ret;
    int  list_calls; int  list_ret;
} P;

static long m_patch_save(void *be, const char *p)
{ (void)be; P.save_calls++; strncpy(P.save_path, p, 63); return P.save_ret; }
static long m_patch_load(void *be, const char *p)
{ (void)be; P.load_calls++; strncpy(P.load_pth, p, 63); return P.load_ret; }
static int  m_patch_list(void *be, const char **names, int max)
{
    (void)be; P.list_calls++;
    if (P.list_ret <= 0) return P.list_ret;
    static const char *canned[] = { "LIVE.PAT", "DRONE.PAT" };
    int n = P.list_ret < max ? P.list_ret : max;
    for (int i = 0; i < n; i++) names[i] = canned[i % 2];
    return n;
}

static const char *m_strerror(void *be, const char *verb, int code)
{
    (void)be; (void)verb;
    switch (code) {
    case -1: return "no such file";
    case -3: return "corrupt patch";
    case -5: return "ABI mismatch";
    default: return 0;              /* fall back to numeric */
    }
}

static shell_graph_ops_t g_ops = {
    m_load, m_unload, m_wire, m_unwire, m_setparam,
    m_describe, m_stats,
    m_patch_save, m_patch_load, m_patch_list,
    m_strerror, 0
};

static void reset_all(shell_t *sh)
{
    memset(&M, 0, sizeof M);
    memset(&P, 0, sizeof P);
    M.load_ret = 7; M.unload_ret = 0; M.wire_ret = 0; M.unwire_ret = 0; M.sp_ret = 0;
    P.save_ret = 0; P.load_ret = 0; P.list_ret = 2;
    shell_graph_install(sh, &g_ops);
    cap_reset();
}

static void run(shell_t *sh, const char *cmdline)
{
    for (const char *p = cmdline; *p; p++)
        shell_feed(sh, *p);
    shell_feed(sh, '\n');
}

/* ---- number parser ---- */
static void test_parse(void)
{
    printf("- sg_parse_u32: decimal, hex, rejects garbage\n");
    uint32_t v;
    CHECK(sg_parse_u32("42", &v) == 0 && v == 42, "decimal");
    CHECK(sg_parse_u32("0x2a", &v) == 0 && v == 0x2a, "hex");
    CHECK(sg_parse_u32("0xFF", &v) == 0 && v == 255, "upper hex");
    CHECK(sg_parse_u32("", &v) != 0, "empty rejected");
    CHECK(sg_parse_u32("12x", &v) != 0, "trailing junk rejected");
    CHECK(sg_parse_u32("0x", &v) != 0, "bare 0x rejected");
}

/* ---- each verb forwards the right arguments ---- */
static void test_verbs(void)
{
    printf("- verbs: arguments forwarded, success lines printed\n");
    shell_t sh;
    shell_init(&sh, 0, 0, cap_out, 0);

    reset_all(&sh);
    run(&sh, "load /sd/sine.elf");
    CHECK(M.load_calls == 1 && strcmp(M.load_path, "/sd/sine.elf") == 0, "load path forwarded");
    CHECK(saw("loaded pid 7"), "load prints the new pid");

    reset_all(&sh);
    run(&sh, "wire 2 3");
    CHECK(M.wire_calls == 1 && M.wire_s == 2 && M.wire_d == 3, "wire pids forwarded");
    CHECK(saw("wired"), "wire success line");

    reset_all(&sh);
    run(&sh, "wire 3 dac");
    CHECK(M.wire_calls == 1 && M.wire_s == 3 && M.wire_d == 0, "'dac' maps to pid 0");

    reset_all(&sh);
    run(&sh, "unwire 2 3");
    CHECK(M.unwire_calls == 1 && M.unwire_s == 2 && M.unwire_d == 3, "unwire forwarded");

    reset_all(&sh);
    run(&sh, "unload 5");
    CHECK(M.unload_calls == 1 && M.unload_pid == 5, "unload pid forwarded");

    /* set-param: decimal value goes through the shared patch parser -> float
     * bits; 880 -> 0x445c0000. */
    reset_all(&sh);
    run(&sh, "set-param 2 0 880");
    CHECK(M.sp_calls == 1 && M.sp_pid == 2 && M.sp_param == 0 &&
          M.sp_bits == 0x445c0000u, "set-param decimal -> float bits");
    reset_all(&sh);
    run(&sh, "set-param 2 1 0xdeadbeef");
    CHECK(M.sp_bits == 0xdeadbeefu, "set-param hex -> raw bits");
}

/* ---- errors: one line, backend told, graph untouched ---- */
static void test_errors(void)
{
    printf("- errors: usage, bad numbers, backend failures\n");
    shell_t sh;
    shell_init(&sh, 0, 0, cap_out, 0);

    reset_all(&sh);
    run(&sh, "load");                       /* missing arg */
    CHECK(M.load_calls == 0 && saw("usage: load"), "missing arg -> usage, no call");

    reset_all(&sh);
    run(&sh, "wire two 3");                  /* bad number */
    CHECK(M.wire_calls == 0 && saw("usage: wire"), "bad number -> usage, no call");

    reset_all(&sh);
    M.load_ret = -1;
    run(&sh, "load /sd/missing.elf");
    CHECK(M.load_calls == 1 && saw("error: load: no such file"),
          "backend error rendered via strerror");

    reset_all(&sh);
    M.sp_ret = -1;
    run(&sh, "set-param 9 0 1");
    CHECK(saw("error: set-param: no such file"), "set-param failure reported");

    reset_all(&sh);
    run(&sh, "set-param 2 0 12.5");          /* not an integer/hex */
    CHECK(M.sp_calls == 0 && saw("bad value"), "bad value rejected before backend");

    /* an unmapped code falls back to the number */
    reset_all(&sh);
    M.unload_ret = -9;
    run(&sh, "unload 3");
    CHECK(saw("error: unload: code -9"), "unmapped code prints numerically");
}

/* ---- ls / stats rendering ---- */
static void test_ls_stats(void)
{
    printf("- ls / stats: rendering\n");
    shell_t sh;
    shell_init(&sh, 0, 0, cap_out, 0);

    reset_all(&sh);
    run(&sh, "ls");
    CHECK(saw("nodes:") && saw("pid 2 synth") && saw("pid 3 filter") && saw("dac"),
          "ls lists nodes incl. dac");
    CHECK(saw("params: 0=0x445c0000"), "ls shows node params");
    CHECK(saw("2 -> 3") && saw("3 -> dac"), "ls lists edges with dac target");

    reset_all(&sh);
    run(&sh, "stats");
    CHECK(saw("audio: serviced=1000 overruns=0"), "stats shows the audio summary");
    CHECK(saw("plugin pid=2 synth runs=500") && saw("mean=12us") && saw("off=0"),
          "stats shows per-plugin service times");
}

/* ---- patch save/load/ls ---- */
static void test_patch(void)
{
    printf("- patch: save / load / ls, and error paths\n");
    shell_t sh;
    shell_init(&sh, 0, 0, cap_out, 0);

    reset_all(&sh);
    run(&sh, "patch save /sd/live.pat");
    CHECK(P.save_calls == 1 && strcmp(P.save_path, "/sd/live.pat") == 0, "save path forwarded");
    CHECK(saw("saved /sd/live.pat"), "save success line");

    reset_all(&sh);
    run(&sh, "patch load /sd/live.pat");
    CHECK(P.load_calls == 1 && strcmp(P.load_pth, "/sd/live.pat") == 0, "load path forwarded");
    CHECK(saw("loaded /sd/live.pat"), "load success line");

    reset_all(&sh);
    run(&sh, "patch ls");
    CHECK(P.list_calls == 1 && saw("patches:") && saw("LIVE.PAT") && saw("DRONE.PAT"),
          "ls lists the patch files");

    reset_all(&sh);
    P.list_ret = 0;
    run(&sh, "patch ls");
    CHECK(saw("(none)"), "empty patch list");

    reset_all(&sh);
    P.load_ret = -3;                        /* corrupt patch */
    run(&sh, "patch load /sd/bad.pat");
    CHECK(saw("error: patch load: corrupt patch"), "corrupt patch reported, no fault");

    reset_all(&sh);
    run(&sh, "patch save");                 /* missing path */
    CHECK(P.save_calls == 0 && saw("usage: patch save"), "missing path -> usage");

    reset_all(&sh);
    run(&sh, "patch frob");                 /* unknown subcommand */
    CHECK(saw("usage: patch"), "unknown subcommand -> usage");
}

int main(void)
{
    printf("=== shell graph command host tests (issue #81) ===\n");
    test_parse();
    test_verbs();
    test_errors();
    test_ls_stats();
    test_patch();

    if (g_fail) {
        printf("SHELL-GRAPH TESTS: %d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("SHELL-GRAPH TESTS: ALL PASS\n");
    return 0;
}
