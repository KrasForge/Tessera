/* tests/arm64/patch_test.c - host unit tests for the patch format (Issue #40).
 *
 * Round-trips a two-plugin graph through serialize/parse, checks the value
 * codec (hex + decimal), and verifies malformed / truncated input is rejected
 * rather than crashing.
 *
 * Build/run via:  make test-arm-patch
 */

#include "patch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static int patches_equal(const patch_t *a, const patch_t *b)
{
    if (a->n_plugins != b->n_plugins || a->n_params != b->n_params ||
        a->n_edges != b->n_edges)
        return 0;
    for (int i = 0; i < a->n_plugins; i++)
        if (strcmp(a->plugins[i].path, b->plugins[i].path) != 0) return 0;
    for (int i = 0; i < a->n_params; i++)
        if (a->params[i].plugin != b->params[i].plugin ||
            a->params[i].id != b->params[i].id ||
            a->params[i].bits != b->params[i].bits) return 0;
    for (int i = 0; i < a->n_edges; i++)
        if (a->edges[i].src != b->edges[i].src ||
            a->edges[i].dst != b->edges[i].dst) return 0;
    return 1;
}

int main(void)
{
    printf("=== Tessera patch-format tests (issue #40) ===\n");

    /* ---- value codec ---- */
    char v[11];
    patch_format_value(0x3f000000u, v);
    CHECK(strcmp(v, "0x3f000000") == 0, "format 0.5 -> 0x3f000000");
    uint32_t bits;
    CHECK(patch_parse_value("0x3f000000", &bits) == PATCH_OK && bits == 0x3f000000u,
          "parse hex 0x3f000000");
    CHECK(patch_parse_value("440", &bits) == PATCH_OK && bits == 0x43dc0000u,
          "parse decimal 440 -> 440.0f bits");
    CHECK(patch_parse_value("-2", &bits) == PATCH_OK && bits == 0xc0000000u,
          "parse decimal -2 -> -2.0f bits");
    CHECK(patch_parse_value("0x", &bits) == PATCH_EFMT, "reject empty hex");
    CHECK(patch_parse_value("12x", &bits) == PATCH_EFMT, "reject bad decimal");

    /* ---- build a two-plugin graph and round-trip it ---- */
    patch_t p;
    patch_init(&p);
    int s = patch_add_plugin(&p, "/sd/synth.elf");
    int e = patch_add_plugin(&p, "/rd/effect");
    CHECK(s == 0 && e == 1, "two plugins added");
    patch_add_param(&p, s, 0u, 0x43dc0000u);      /* synth freq 440 */
    patch_add_param(&p, e, 2u, 0x3f000000u);      /* effect mix 0.5 */
    patch_add_edge(&p, s, e);                      /* synth -> effect */
    patch_add_edge(&p, e, PATCH_DAC);              /* effect -> DAC   */

    char text[512];
    long n = patch_serialize(&p, text, sizeof(text));
    CHECK(n > 0, "serialize succeeds");
    printf("---- serialized ----\n%s--------------------\n", text);

    patch_t q;
    CHECK(patch_parse(text, (uint32_t)n, &q) == PATCH_OK, "parse the serialized text");
    CHECK(patches_equal(&p, &q), "round-trip is identical");
    CHECK(q.edges[1].dst == PATCH_DAC, "DAC edge preserved");

    /* ---- editability: a hand-edited decimal value parses ---- */
    const char *edited =
        "# my tweak\n"
        "plugin /sd/synth.elf\n"
        "param 0 0 880\n"                  /* changed 440 -> 880 by hand */
        "connect 0 dac\n";
    patch_t r;
    CHECK(patch_parse(edited, (uint32_t)strlen(edited), &r) == PATCH_OK,
          "hand-edited patch parses");
    CHECK(r.n_params == 1 && r.params[0].bits == 0x445c0000u,
          "edited decimal 880 -> 880.0f bits");

    /* ---- corrupt / truncated inputs are rejected, never crash ---- */
    CHECK(patch_parse("plugin\n", 7, &r) == PATCH_ETRUNC, "truncated plugin line");
    CHECK(patch_parse("param 0 1\n", 10, &r) == PATCH_ETRUNC, "truncated param line");
    CHECK(patch_parse("connect 0\n", 10, &r) == PATCH_ETRUNC, "truncated connect line");
    CHECK(patch_parse("frobnicate 1 2\n", 15, &r) == PATCH_EFMT, "unknown keyword");
    CHECK(patch_parse("plugin /sd/a.elf\nparam 5 0 0x1\n", 30, &r) == PATCH_ERANGE,
          "param references out-of-range plugin");
    /* A file truncated mid-token (no newline) must not read past the buffer. */
    CHECK(patch_parse("plugin /sd/synth.elf\nparam 0 1 0x3f0", 36, &r) == PATCH_OK,
          "value token at EOF without newline is read safely");

    /* ---- serialize into a too-small buffer ---- */
    char tiny[8];
    CHECK(patch_serialize(&p, tiny, sizeof(tiny)) == PATCH_ENOSPACE,
          "serialize reports ENOSPACE for a small buffer");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
