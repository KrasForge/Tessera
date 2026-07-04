/* tests/arm64/fuzz_parsers.c - shared fuzz harness over the untrusted parsers
 * (Theme M16, issue #169).
 *
 * Tessera parses several byte streams from untrusted sources - OSC editor
 * messages (#123), USB Audio descriptors (#133), signed packages (#125), the
 * VideoCore mailbox (#105), and embedded presets (#127).  Each is individually
 * bounds-checked; this feeds every one of them a flood of random and mutated
 * bytes under AddressSanitizer + UBSan, so an out-of-bounds read, an overflow, or
 * a non-terminating loop trips the sanitizer and fails the build.
 *
 * It is a self-contained, *deterministic* fuzzer (a seeded LCG, a fixed
 * iteration budget) so it needs no libFuzzer/clang and runs the same everywhere.
 * Adding a parser is one entry in the `targets` table.
 *
 * Build/run via:  make test-arm-fuzz
 */

#include "osc.h"
#include "usbaudio.h"
#include "package.h"
#include "mailbox.h"
#include "presets.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- deterministic RNG --------------------------------------------------- */

static uint32_t g_rng = 0xC0FFEEu;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

#define MAXBUF 512

/* ---- parser targets: each consumes arbitrary bytes ----------------------- */

static void t_osc(const uint8_t *d, int len)
{
    osc_message_t m;
    if (osc_parse(d, len, &m) == 0) {
        osc_cmd_t cmd;
        osc_editor_dispatch(&m, &cmd);          /* walk args too */
    }
}

static void t_usb(const uint8_t *d, int len)
{
    usb_audio_format_t f;
    if (usb_audio_parse_format(d, len, &f) == 0)
        (void)usb_audio_supports_rate(&f, 48000);
}

static const uint8_t FUZZ_KEY[8] = { 'k','e','y',0,1,2,3,4 };

static void t_pkg(const uint8_t *d, int len)
{
    pkg_info_t info;
    uint32_t revoked[2] = { 1, 7 };
    (void)pkg_verify(d, (size_t)len, FUZZ_KEY, sizeof FUZZ_KEY, revoked, 2, &info);
}

static void t_mbox(const uint8_t *d, int len)
{
    /* The mailbox works on uint32 words; view the bytes as a word buffer. */
    int words = len / 4;
    if (words < 1) return;
    const uint32_t *buf = (const uint32_t *)(const void *)d;
    const uint32_t *val; int nw;
    (void)mbox_find_tag(buf, words, 0x00010002u, &val, &nw);
    (void)mbox_response_ok(buf);
}

static void t_presets(const uint8_t *d, int len)
{
    preset_table_t t;
    if (presets_open(&t, d, (uint32_t)len) == 0) {
        int n = presets_count(&t);
        for (int i = 0; i < n; i++) {
            preset_info_t p;
            if (presets_get(&t, i, &p) == 0)
                for (int k = 0; k < (int)p.n_params; k++) {
                    uint32_t id, bits;
                    preset_param(&p, k, &id, &bits);
                }
        }
    }
}

typedef void (*target_fn)(const uint8_t *, int);
static const struct { const char *name; target_fn fn; } targets[] = {
    { "osc",     t_osc },
    { "usb",     t_usb },
    { "package", t_pkg },
    { "mailbox", t_mbox },
    { "presets", t_presets },
};
#define N_TARGETS ((int)(sizeof targets / sizeof targets[0]))

/* ---- seed corpus (valid inputs to mutate) -------------------------------- */

static _Alignas(8) uint8_t g_seed[N_TARGETS][MAXBUF];   /* 8-aligned for mailbox words */
static int      g_seed_len[N_TARGETS];

static void build_seeds(void)
{
    /* OSC: a valid /tessera/param ,iii message. */
    {
        osc_arg_t a[3] = { {OSC_INT32,{.i=1}}, {OSC_INT32,{.i=2}}, {OSC_INT32,{.i=3}} };
        g_seed_len[0] = osc_encode(g_seed[0], MAXBUF, "/tessera/param", a, 3);
    }
    /* USB: a valid UAC1 Format Type I descriptor. */
    {
        static const uint8_t desc[] = {
            0x0e,0x24,0x02,0x01, 0x02,0x02,0x10,0x02, 0x44,0xac,0x00, 0x80,0xbb,0x00 };
        memcpy(g_seed[1], desc, sizeof desc); g_seed_len[1] = sizeof desc;
    }
    /* Package: a valid signed package. */
    {
        static const uint8_t payload[] = { 1,2,3,4,5,6,7,8 };
        g_seed_len[2] = pkg_build(g_seed[2], MAXBUF, 0x0101u, 3, 0, "fx",
                                  payload, sizeof payload, FUZZ_KEY, sizeof FUZZ_KEY);
    }
    /* Mailbox: a valid property message. */
    {
        mbox_builder_t b; mbox_init(&b, (uint32_t *)(void *)g_seed[3], MAXBUF / 4);
        uint32_t clk[2] = { 5, 48000u * 256u };
        mbox_add_tag(&b, 0x00010002u, 0, 0, 1);
        mbox_add_tag(&b, 0x00038002u, clk, 2, 2);
        int words = mbox_finish(&b);
        g_seed_len[3] = words > 0 ? words * 4 : 0;
    }
    /* Presets: a valid two-preset blob. */
    {
        int off = presets_build_header(g_seed[4], MAXBUF, 2);
        preset_param_t p1[1] = { { 0, 0x3f800000u } };
        preset_param_t p2[2] = { { 0, 0x40000000u }, { 1, 0x3f000000u } };
        off = presets_build_add(g_seed[4], MAXBUF, off, "A", p1, 1);
        off = presets_build_add(g_seed[4], MAXBUF, off, "B", p2, 2);
        g_seed_len[4] = off > 0 ? off : 0;
    }
}

/* Fill `buf` with either fully random bytes or a mutated copy of a seed. */
static int gen_input(int ti, uint8_t *buf)
{
    uint32_t mode = rnd() % 3u;
    if (mode == 0 || g_seed_len[ti] <= 0) {
        int len = (int)(rnd() % (MAXBUF + 1));
        for (int i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        return len;
    }
    /* Mutate the seed: copy, then truncate/extend and flip a few bytes. */
    int len = g_seed_len[ti];
    memcpy(buf, g_seed[ti], len);
    if (mode == 2) {                              /* resize */
        int delta = (int)(rnd() % 33u) - 16;
        len += delta;
        if (len < 0) len = 0;
        if (len > MAXBUF) len = MAXBUF;
        for (int i = g_seed_len[ti]; i < len; i++) buf[i] = (uint8_t)rnd();
    }
    int flips = 1 + (int)(rnd() % 6u);
    for (int f = 0; f < flips && len > 0; f++)
        buf[rnd() % (uint32_t)len] = (uint8_t)rnd();
    return len;
}

int main(int argc, char **argv)
{
    int rounds = (argc > 1) ? atoi(argv[1]) : 60000;
    printf("=== Tessera parser fuzz harness (M16, #169): %d rounds x %d parsers ===\n",
           rounds, N_TARGETS);
    build_seeds();

    /* 8-byte alignment for the uint32 mailbox view. */
    static _Alignas(8) uint8_t buf[MAXBUF];
    long fed[N_TARGETS] = {0};
    for (int r = 0; r < rounds; r++) {
        for (int ti = 0; ti < N_TARGETS; ti++) {
            int len = gen_input(ti, buf);
            targets[ti].fn(buf, len);
            fed[ti]++;
        }
    }

    for (int ti = 0; ti < N_TARGETS; ti++)
        printf("  ok   : %-8s survived %ld inputs (no ASan/UBSan finding)\n",
               targets[ti].name, fed[ti]);
    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
