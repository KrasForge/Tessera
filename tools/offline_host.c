/* tools/offline_host.c - offline plugin host (Theme G, issue #128)
 *
 * Runs a Tessera plugin against a WAV file on the desktop, so an author can
 * iterate without a board or QEMU.  It links the plugin's C directly (the same
 * host-compiled path the plugin DSP tests use) and drives its ABI entry points
 * block by block:
 *
 *     offline_host in.wav out.wav [automation.csv]
 *
 * `automation.csv` is optional: lines of `frame,param_id,value` schedule
 * plugin_set_param() calls at frame boundaries, so effects can be swept over
 * time.  `offline_host --selftest` needs no files: it renders a known signal
 * through the linked plugin and checks the WAV round-trip and that the plugin
 * altered the audio as expected.
 *
 * The plugin under test is chosen at build time by which plugin source is
 * linked (see the `offline-host` / `test-arm-offline-host` Makefile targets,
 * which build it against the reference low-pass).  Host tool: malloc/stdio are
 * fine here; only the plugin's process_block obeys the real-time rules.
 */

#include "plugin_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define BLOCK 256u
#define PI_D  3.14159265358979323846

/* ---- minimal 16-bit PCM WAV I/O ----------------------------------------- */

typedef struct {
    uint32_t sr;
    uint16_t ch;        /* 1 or 2 */
    uint32_t frames;
    int16_t *pcm;       /* interleaved, ch * frames samples */
} wav_t;

static void put_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }
static uint32_t get_u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t get_u16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

static int wav_read(const char *path, wav_t *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "offline_host: cannot open %s\n", path); return 0; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "offline_host: %s is not a RIFF/WAVE file\n", path); fclose(f); return 0;
    }
    uint16_t fmt = 0, ch = 0, bits = 0; uint32_t sr = 0;
    long data_off = -1; uint32_t data_len = 0;
    uint8_t ch4[8];
    while (fread(ch4, 1, 8, f) == 8) {
        uint32_t sz = get_u32(ch4 + 4);
        if (!memcmp(ch4, "fmt ", 4)) {
            uint8_t fb[16]; if (fread(fb, 1, 16, f) != 16) break;
            fmt = get_u16(fb); ch = get_u16(fb + 2); sr = get_u32(fb + 4); bits = get_u16(fb + 14);
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(ch4, "data", 4)) {
            data_off = ftell(f); data_len = sz; fseek(f, sz, SEEK_CUR);
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);   /* skip, chunks are word-aligned */
        }
    }
    if (fmt != 1 || bits != 16 || (ch != 1 && ch != 2) || data_off < 0) {
        fprintf(stderr, "offline_host: only 16-bit PCM mono/stereo WAV supported\n"); fclose(f); return 0;
    }
    w->sr = sr; w->ch = ch; w->frames = data_len / (2u * ch);
    w->pcm = malloc((size_t)w->frames * ch * sizeof(int16_t));
    if (!w->pcm) { fclose(f); return 0; }
    fseek(f, data_off, SEEK_SET);
    for (uint32_t i = 0; i < w->frames * ch; i++) {
        uint8_t s[2]; if (fread(s, 1, 2, f) != 2) { break; }
        w->pcm[i] = (int16_t)get_u16(s);
    }
    fclose(f);
    return 1;
}

static int wav_write(const char *path, const wav_t *w)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "offline_host: cannot write %s\n", path); return 0; }
    uint32_t data_len = w->frames * w->ch * 2u;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_len); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 16); put_u16(f, 1); put_u16(f, w->ch);
    put_u32(f, w->sr); put_u32(f, w->sr * w->ch * 2u); put_u16(f, (uint16_t)(w->ch * 2u)); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, data_len);
    for (uint32_t i = 0; i < w->frames * w->ch; i++) put_u16(f, (uint16_t)w->pcm[i]);
    fclose(f);
    return 1;
}

static int16_t f2s(float v)
{
    float s = v * 32768.0f;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (int16_t)lrintf(s);
}

/* ---- parameter automation (frame,id,value) ------------------------------ */

typedef struct { uint32_t frame, id; float value; } autom_t;

static int load_autom(const char *path, autom_t **out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "offline_host: cannot open %s\n", path); return -1; }
    int cap = 16, n = 0; autom_t *a = malloc(cap * sizeof *a);
    uint32_t fr, id; float v;
    while (fscanf(f, " %u , %u , %f", &fr, &id, &v) == 3) {
        if (n == cap) { cap *= 2; a = realloc(a, cap * sizeof *a); }
        a[n].frame = fr; a[n].id = id; a[n].value = v; n++;
    }
    fclose(f);
    *out = a;
    return n;
}

/* ---- drive the plugin over a WAV ---------------------------------------- */

static void process(wav_t *in, wav_t *out, const autom_t *autom, int n_autom)
{
    out->sr = in->sr; out->ch = in->ch; out->frames = in->frames;
    out->pcm = malloc((size_t)in->frames * in->ch * sizeof(int16_t));

    plugin_init(in->sr, BLOCK);
    float il[BLOCK], ir[BLOCK], ol[BLOCK], orr[BLOCK];
    int ai = 0;

    for (uint32_t base = 0; base < in->frames; base += BLOCK) {
        uint32_t n = in->frames - base; if (n > BLOCK) n = BLOCK;
        while (ai < n_autom && autom[ai].frame < base + n) {
            plugin_set_param(autom[ai].id, autom[ai].value);
            ai++;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (base + i) * in->ch;
            il[i] = in->pcm[idx] / 32768.0f;
            ir[i] = (in->ch == 2) ? in->pcm[idx + 1] / 32768.0f : il[i];
        }
        plugin_process_block(il, ir, ol, orr, n);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (base + i) * out->ch;
            out->pcm[idx] = f2s(ol[i]);
            if (out->ch == 2) out->pcm[idx + 1] = f2s(orr[i]);
        }
    }
    plugin_destroy();
}

/* ---- self-test (no files needed) ---------------------------------------- */

static double hf_energy(const int16_t *p, uint32_t frames, uint16_t ch)
{
    double acc = 0.0;
    for (uint32_t i = 1; i < frames; i++) {
        double d = (double)p[i * ch] - (double)p[(i - 1) * ch];   /* first difference ~ HF */
        acc += d < 0 ? -d : d;
    }
    return acc / (frames - 1);
}

static int selftest(void)
{
    printf("=== offline_host self-test (Theme G, #128) ===\n");
    int fail = 0;
    #define CK(c, m) do { if (c) printf("  ok   : %s\n", m); else { printf("  FAIL : %s\n", m); fail++; } } while (0)

    /* a stereo signal: a low tone plus a strong high tone */
    uint32_t frames = 48000, sr = 48000;
    wav_t in = { sr, 2, frames, malloc(frames * 2 * sizeof(int16_t)) };
    for (uint32_t i = 0; i < frames; i++) {
        double t = (double)i / sr;
        double v = 0.30 * sin(2 * PI_D * 200.0 * t) + 0.30 * sin(2 * PI_D * 15000.0 * t);
        int16_t s = f2s((float)v);
        in.pcm[i * 2] = s; in.pcm[i * 2 + 1] = s;
    }

    /* WAV round-trip is bit-exact */
    CK(wav_write("/tmp/offline_host_in.wav", &in), "write input WAV");
    wav_t rt;
    CK(wav_read("/tmp/offline_host_in.wav", &rt), "read it back");
    int exact = rt.frames == in.frames && rt.ch == in.ch && rt.sr == in.sr;
    for (uint32_t i = 0; exact && i < frames * 2; i++) if (rt.pcm[i] != in.pcm[i]) exact = 0;
    CK(exact, "WAV round-trip is bit-exact");

    /* run the linked plugin (the low-pass) with a 1 kHz cutoff */
    autom_t a = { 0, 0, 1000.0f };   /* frame 0: param 0 (cutoff) = 1000 Hz */
    wav_t out;
    process(&in, &out, &a, 1);
    CK(wav_write("/tmp/offline_host_out.wav", &out), "write output WAV");

    double hin = hf_energy(in.pcm, frames, 2), hout = hf_energy(out.pcm, frames, 2);
    printf("    HF energy: in=%.1f out=%.1f\n", hin, hout);
    CK(hout < hin * 0.5, "the low-pass reduced the high-frequency energy");
    int changed = 0;
    for (uint32_t i = 0; i < frames * 2; i++) if (out.pcm[i] != in.pcm[i]) { changed = 1; break; }
    CK(changed, "the plugin altered the audio");

    free(in.pcm); free(rt.pcm); free(out.pcm);
    printf("=== %s ===\n", fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return selftest();
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.wav out.wav [automation.csv]\n"
                        "       %s --selftest\n", argv[0], argv[0]);
        return 2;
    }
    wav_t in;
    if (!wav_read(argv[1], &in)) return 1;
    autom_t *autom = NULL; int n_autom = 0;
    if (argc >= 4) { n_autom = load_autom(argv[3], &autom); if (n_autom < 0) return 1; }
    wav_t out;
    process(&in, &out, autom, n_autom);
    if (!wav_write(argv[2], &out)) return 1;
    printf("offline_host: %u frames, %u Hz, %u ch -> %s\n", in.frames, in.sr, in.ch, argv[2]);
    free(in.pcm); free(out.pcm); free(autom);
    return 0;
}
