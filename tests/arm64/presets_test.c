/* tests/arm64/presets_test.c - host unit tests for embedded presets + config
 * negotiation (Theme F, issue #127).
 *
 * Build/run via:  make test-arm-presets
 */

#include "presets.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Build a two-preset blob into `buf`; returns its length. */
static int make_blob(uint8_t *buf, int cap)
{
    int off = presets_build_header(buf, cap, 2);
    preset_param_t clean[] = { { 0, 0x3f800000u }, { 1, 0x00000000u } };
    preset_param_t lead[]  = { { 0, 0x40000000u }, { 1, 0x3f000000u }, { 2, 0x41200000u } };
    off = presets_build_add(buf, cap, off, "Clean", clean, 2);
    off = presets_build_add(buf, cap, off, "Lead",  lead,  3);
    return off;
}

static void test_read(void)
{
    printf("- build, open, and read embedded presets\n");
    uint8_t buf[256];
    int len = make_blob(buf, sizeof buf);
    CHECK(len > 0, "blob built");

    preset_table_t t;
    CHECK(presets_open(&t, buf, len) == 0, "open ok");
    CHECK(presets_count(&t) == 2, "two presets");

    preset_info_t p;
    CHECK(presets_get(&t, 0, &p) == 0 && strcmp(p.name, "Clean") == 0 && p.n_params == 2,
          "preset 0 is Clean with 2 params");
    uint32_t id, bits;
    CHECK(preset_param(&p, 0, &id, &bits) == 0 && id == 0 && bits == 0x3f800000u,
          "Clean param 0 = (0, 1.0f-bits)");

    CHECK(presets_get(&t, 1, &p) == 0 && strcmp(p.name, "Lead") == 0 && p.n_params == 3,
          "preset 1 is Lead with 3 params");
    CHECK(preset_param(&p, 2, &id, &bits) == 0 && id == 2 && bits == 0x41200000u,
          "Lead param 2 = (2, 10.0f-bits)");
    CHECK(preset_param(&p, 3, &id, &bits) == -1, "reading past the params fails");

    CHECK(presets_get(&t, 2, &p) == -1, "out-of-range preset index fails");
}

static void test_malformed(void)
{
    printf("- malformed blobs are rejected without over-reading\n");
    preset_table_t t;
    uint8_t buf[256];
    int len = make_blob(buf, sizeof buf);

    CHECK(presets_open(&t, buf, 4) == -1, "too short for a header");
    uint8_t bad[256]; memcpy(bad, buf, len);
    bad[0] ^= 0xff;
    CHECK(presets_open(&t, bad, len) == -1, "bad magic rejected");
    memcpy(bad, buf, len);
    bad[4] = 9;                                   /* wrong version */
    CHECK(presets_open(&t, bad, len) == -1, "bad version rejected");

    /* Truncate mid-second-preset: open still succeeds (header intact) but
     * reading the second preset must fail rather than over-read. */
    CHECK(presets_open(&t, buf, len - 4) == 0, "header still opens");
    preset_info_t p;
    CHECK(presets_get(&t, 0, &p) == 0, "first preset still readable");
    CHECK(presets_get(&t, 1, &p) == -1, "truncated second preset is rejected");
}

static void test_negotiate(void)
{
    printf("- sample-rate / block-size negotiation\n");
    plugin_caps_t caps = { .rates = { 44100, 48000, 96000 }, .n_rates = 3,
                           .block_min = 32, .block_max = 512 };

    CHECK(caps_supports_rate(&caps, 48000) == 1, "48k is supported");
    CHECK(caps_supports_rate(&caps, 22050) == 0, "22.05k is not");
    CHECK(caps_supports_block(&caps, 64) == 1, "block 64 is in range");
    CHECK(caps_supports_block(&caps, 1024) == 0, "block 1024 is too big");

    uint32_t sr, blk;
    /* Directly acceptable config -> accepted verbatim, returns 1. */
    CHECK(caps_negotiate(&caps, 48000, 128, &sr, &blk) == 1 && sr == 48000 && blk == 128,
          "acceptable config is used as-is");

    /* Unsupported rate -> nearest advertised (44100 is closest to 44000). */
    CHECK(caps_negotiate(&caps, 44000, 128, &sr, &blk) == 0 && sr == 44100,
          "unsupported rate snaps to the nearest supported one");

    /* Oversized block -> clamped to block_max, returns 0 (not verbatim). */
    CHECK(caps_negotiate(&caps, 48000, 4096, &sr, &blk) == 0 && sr == 48000 && blk == 512,
          "oversized block is clamped to block_max");

    /* Undersized block -> clamped up to block_min. */
    CHECK(caps_negotiate(&caps, 48000, 8, &sr, &blk) == 0 && blk == 32,
          "undersized block is clamped to block_min");

    /* A plugin that accepts anything (no rate list, no block bounds). */
    plugin_caps_t any = { .n_rates = 0, .block_min = 0, .block_max = 0 };
    CHECK(caps_negotiate(&any, 12345, 999, &sr, &blk) == 1 && sr == 12345 && blk == 999,
          "an any-config plugin accepts whatever the host offers");
}

int main(void)
{
    printf("=== Tessera embedded-presets / negotiation tests (Theme F, #127) ===\n");
    test_read();
    test_malformed();
    test_negotiate();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
