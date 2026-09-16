#include "sample_bits.h"
#include "session_preset.h"
#include "shell.h"
#include "shell_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, called;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "CODEC FAIL %d: %s\n", __LINE__, #x);                                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static int command(shell_t *s, int n, char **v)
{
    (void)s;
    (void)n;
    (void)v;
    ++called;
    return 0;
}
static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
int main(void)
{
    uint64_t u = 42;
    uint32_t small = 7;
    CHECK(!session_parse_u64("18446744073709551615", &u) && u == UINT64_MAX);
    CHECK(session_parse_u64("18446744073709551616", &u) < 0 && u == UINT64_MAX);
    CHECK(!sg_parse_u32("4294967295", &small) && small == UINT32_MAX);
    CHECK(sg_parse_u32("4294967296", &small) < 0 && small == UINT32_MAX);
    CHECK(sg_parse_u32("0x100000001", &small) < 0);
    CHECK(patch_parse_value("0x100000000", &small) < 0);
    CHECK(patch_parse_value("4294967296", &small) < 0);
    CHECK(!patch_parse_value("0.5", &small) && small == 0x3f000000);
    CHECK(!patch_parse_value("-0.25", &small) && small == 0xbe800000);
    CHECK(!patch_parse_value(".125", &small) && small == 0x3e000000);
    CHECK(!patch_parse_value("0.1", &small) && small == 0x3dcccccd);
    CHECK(patch_parse_value("1.2.3", &small) < 0);
    CHECK(patch_parse_value("0.1234567890", &small) < 0);
    CHECK(patch_parse_value("1.", &small) < 0);
    shell_t sh;
    const shell_cmd_t cmds[] = {{"clear", "", command}};
    shell_init(&sh, cmds, 1, NULL, NULL);
    const char *prefix = "clear ";
    for (const char *c = prefix; *c; ++c)
        shell_feed(&sh, *c);
    for (unsigned i = 0; i < 1000; ++i)
        shell_feed(&sh, ' ');
    shell_feed(&sh, '\n');
    CHECK(!called && sh.last_result == SHELL_EOVERFLOW);
    for (const char *c = prefix; *c; ++c)
        shell_feed(&sh, *c);
    for (unsigned i = 0; i < 18; ++i) {
        shell_feed(&sh, 'x');
        shell_feed(&sh, ' ');
    }
    shell_feed(&sh, '\n');
    CHECK(!called && sh.last_result == SHELL_EOVERFLOW);
    for (const char *c = "clear\n"; *c; ++c)
        shell_feed(&sh, *c);
    CHECK(called == 1);
    CHECK(sample_q31(0x3f000000) == 1073741824 && sample_q31(0xbf800000) == INT32_MIN);
    CHECK(sample_q31(0x7fc00000) == 0 && sample_q31(0x7f800000) == 0);
    CHECK(sample_float(1073741824) == 0x3f000000 && sample_float(INT32_MIN) == 0xbf800000);
    session_preset_t p = {0}, decoded = {0};
    p.sample_rate = 48000;
    p.frames = 64;
    p.counter_hz = 62500000;
    p.cores = 3;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(patch_add_plugin(&p.patch, i == 0 ? "/sd/SYNTH.ELF" : "/sd/GAIN.ELF") >= 0);
        p.contract[i] = (temporal_contract_t){.pid = i + 1,
                                              .period = 1000,
                                              .deadline = 980,
                                              .cpu_budget = 400,
                                              .criticality = TC_HARD,
                                              .overrun_policy = TC_MUTE};
        p.pin[i] = i + 1;
    }
    CHECK(patch_add_param(&p.patch, 1, 0, 0x3f000000) >= 0);
    CHECK(patch_add_edge(&p.patch, 0, 1) >= 0);
    CHECK(patch_add_edge(&p.patch, 1, PATCH_DAC) >= 0);
    char text[SESSION_TEXT_MAX];
    long n = session_preset_write(&p, text, sizeof text);
    CHECK(n > 0);
    CHECK(!session_preset_parse(text, (uint32_t)n, &decoded) && !memcmp(&p, &decoded, sizeof p));
    tm_limits_t lim = {{1000, 10, 10}, 10};
    tm_plan_t plan;
    CHECK(session_preset_plan(&p, &lim, &plan, NULL) == TC_OK);
    p.cores = 1;
    CHECK(session_preset_plan(&p, &lim, &plan, NULL) < 0);
    p.cores = 3;
    session_preset_t old = decoded;
    for (long cut = 0; cut < n; ++cut) {
        int rc = session_preset_parse(text, (uint32_t)cut, &decoded);
        if (rc != 0)
            CHECK(!memcmp(&old, &decoded, sizeof old));
        else
            old = decoded;
    }
    decoded = p;
    old = decoded;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        char fuzz[256];
        unsigned len = rnd() % 256;
        for (unsigned j = 0; j < len; ++j)
            fuzz[j] = (char)rnd();
        int rc = session_preset_parse(fuzz, len, &decoded);
        if (rc != 0)
            CHECK(!memcmp(&old, &decoded, sizeof old));
        else
            old = decoded;
    }
    p.patch.n_params = 1;
    p.patch.params[0].plugin = 15;
    CHECK(session_preset_plan(&p, &lim, &plan, NULL) == SESSION_EFORMAT);
    p.patch.n_params = 129;
    CHECK(session_preset_plan(&p, &lim, &plan, NULL) == SESSION_EFORMAT);
    CHECK(session_preset_write(&p, text, 1) < 0);
    printf("SESSION CODEC: PASS (%u checks)\n", checks);
    return 0;
}
