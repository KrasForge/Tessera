/* tests/arm64/osc_test.c - host unit tests for the OSC codec + remote-editor
 * dispatch (Theme E, issue #123).
 *
 * OSC 1.0 has an exact byte layout, so the tests check encoded bytes directly,
 * round-trip encode->parse, reject malformed input, and confirm editor messages
 * map to the right commands.
 *
 * Build/run via:  make test-arm-osc
 */

#include "osc.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_encode_layout(void)
{
    printf("- encoded bytes match the OSC 1.0 layout\n");
    uint8_t buf[64];
    osc_arg_t args[1] = { { OSC_INT32, { .i = 0x01020304 } } };
    int n = osc_encode(buf, sizeof buf, "/ab", args, 1);
    /* "/ab\0" (4) + ",i\0\0" (4) + int32 BE (4) = 12 bytes. */
    CHECK(n == 12, "length is 12 and 4-byte aligned");
    CHECK(memcmp(buf, "/ab\0", 4) == 0, "address padded with a NUL");
    CHECK(memcmp(buf + 4, ",i\0\0", 4) == 0, "type-tag string ',i' padded");
    CHECK(buf[8] == 0x01 && buf[9] == 0x02 && buf[10] == 0x03 && buf[11] == 0x04,
          "int32 is big-endian");

    /* A 4-char address needs a whole extra pad word of NULs, and the required
     * empty type-tag string "," follows (OSC 1.0 always emits it). */
    int m = osc_encode(buf, sizeof buf, "/abc", NULL, 0);
    CHECK(m == 12 && memcmp(buf, "/abc\0\0\0\0", 8) == 0 && memcmp(buf + 8, ",\0\0\0", 4) == 0,
          "a 4-char address pads to 8, then the empty ',' tag string");
}

static void test_roundtrip(void)
{
    printf("- encode -> parse round-trips address, tags, and args\n");
    uint8_t buf[128];
    osc_arg_t args[3] = {
        { OSC_INT32,   { .i = -42 } },
        { OSC_FLOAT32, { .f = 0x3f800000u } },   /* 1.0f as bits */
        { OSC_STRING,  { .s = "hello" } },
    };
    int n = osc_encode(buf, sizeof buf, "/tessera/x", args, 3);
    CHECK(n > 0, "encode succeeds");

    osc_message_t m;
    CHECK(osc_parse(buf, n, &m) == 0, "parse succeeds");
    CHECK(strcmp(m.address, "/tessera/x") == 0, "address preserved");
    CHECK(m.n_args == 3, "three args");
    CHECK(m.args[0].type == OSC_INT32 && m.args[0].v.i == -42, "int32 preserved");
    CHECK(m.args[1].type == OSC_FLOAT32 && m.args[1].v.f == 0x3f800000u,
          "float bit pattern preserved (no FP touched)");
    CHECK(m.args[2].type == OSC_STRING && strcmp(m.args[2].v.s, "hello") == 0,
          "string preserved");
}

static void test_reject_malformed(void)
{
    printf("- malformed / out-of-bounds input is rejected\n");
    osc_message_t m;
    /* Not 4-byte aligned. */
    CHECK(osc_parse((const uint8_t *)"/x\0", 3, &m) == -1, "unaligned length rejected");
    /* Address not starting with '/'. */
    CHECK(osc_parse((const uint8_t *)"abc\0", 4, &m) == -1, "address must start with '/'");
    /* Type tag string claims an int32 arg but the buffer ends. */
    uint8_t trunc[8] = { '/','a',0,0, ',','i',0,0 };   /* no int32 payload */
    CHECK(osc_parse(trunc, 8, &m) == -1, "truncated int32 arg rejected");
    /* Unterminated address (no NUL within bounds). */
    uint8_t noterm[4] = { '/','a','b','c' };
    CHECK(osc_parse(noterm, 4, &m) == -1, "unterminated address rejected");
}

static void test_editor_dispatch(void)
{
    printf("- editor messages map to commands\n");
    uint8_t buf[128];
    osc_message_t m;
    osc_cmd_t cmd;

    /* /tessera/param ,iii 1 5 0x43dc0000  (440.0f bits as an int) */
    osc_arg_t p[3] = {
        { OSC_INT32, { .i = 1 } },
        { OSC_INT32, { .i = 5 } },
        { OSC_INT32, { .i = (int32_t)0x43dc0000 } },
    };
    int n = osc_encode(buf, sizeof buf, "/tessera/param", p, 3);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) && cmd.type == OSC_CMD_SET_PARAM,
          "/tessera/param ,iii -> SET_PARAM");
    CHECK(cmd.plugin == 1 && cmd.id == 5 && cmd.value_bits == 0x43dc0000u,
          "param fields decoded");

    /* Float form ,iif carries the value as a bit pattern too. */
    osc_arg_t pf[3] = {
        { OSC_INT32,   { .i = 2 } },
        { OSC_INT32,   { .i = 0 } },
        { OSC_FLOAT32, { .f = 0x3f000000u } },   /* 0.5f */
    };
    n = osc_encode(buf, sizeof buf, "/tessera/param", pf, 3);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) && cmd.value_bits == 0x3f000000u,
          "/tessera/param ,iif -> SET_PARAM with float bits");

    /* /tessera/connect ,ii 0 1 */
    osc_arg_t c[2] = { { OSC_INT32, { .i = 0 } }, { OSC_INT32, { .i = 1 } } };
    n = osc_encode(buf, sizeof buf, "/tessera/connect", c, 2);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) && cmd.type == OSC_CMD_CONNECT &&
          cmd.src == 0 && cmd.dst == 1, "/tessera/connect ,ii -> CONNECT");

    /* /tessera/load ,s /sd/x.patch */
    osc_arg_t l[1] = { { OSC_STRING, { .s = "/sd/x.patch" } } };
    n = osc_encode(buf, sizeof buf, "/tessera/load", l, 1);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) && cmd.type == OSC_CMD_LOAD &&
          strcmp(cmd.path, "/sd/x.patch") == 0, "/tessera/load ,s -> LOAD");

    /* /tessera/ping (no args) */
    n = osc_encode(buf, sizeof buf, "/tessera/ping", NULL, 0);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) && cmd.type == OSC_CMD_PING,
          "/tessera/ping -> PING");

    /* Wrong type tags for a known address are not dispatched. */
    osc_arg_t bad[2] = { { OSC_INT32, { .i = 1 } }, { OSC_STRING, { .s = "x" } } };
    n = osc_encode(buf, sizeof buf, "/tessera/param", bad, 2);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) == 0, "/tessera/param with wrong tags is ignored");

    /* Unknown address is not dispatched. */
    n = osc_encode(buf, sizeof buf, "/tessera/unknown", NULL, 0);
    osc_parse(buf, n, &m);
    CHECK(osc_editor_dispatch(&m, &cmd) == 0, "unknown address is ignored");
}

int main(void)
{
    printf("=== Tessera OSC editor-protocol tests (Theme E, #123) ===\n");
    test_encode_layout();
    test_roundtrip();
    test_reject_malformed();
    test_editor_dispatch();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
