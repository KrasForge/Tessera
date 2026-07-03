/* arch/arm64/patch.c - preset/patch file format (Issue #40, M9).
 *
 * Pure text <-> model.  No floating point (parameter values are carried as
 * their 32-bit bit patterns), no allocation, bounds-checked against the input
 * length so a truncated or corrupt file is rejected rather than crashing. */

#include "patch.h"

/* ---- small string helpers ------------------------------------------------ */

static int streq(const char *a, const char *b)
{
    for (int i = 0; i < PATCH_PATH_MAX + 8; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 0;
}

/* Append NUL-terminated `s` to out[*pos], bounded by max.  Returns 0 or -1. */
static int put(char *out, uint32_t *pos, uint32_t max, const char *s)
{
    for (int i = 0; s[i]; i++) {
        if (*pos + 1 >= max) return -1;
        out[(*pos)++] = s[i];
    }
    return 0;
}

static int put_uint(char *out, uint32_t *pos, uint32_t max, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    for (int i = n - 1; i >= 0; i--) {
        if (*pos + 1 >= max) return -1;
        out[(*pos)++] = tmp[i];
    }
    return 0;
}

/* ---- value codec (no FP) ------------------------------------------------- */

void patch_format_value(uint32_t bits, char out[11])
{
    static const char hex[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = hex[(bits >> ((7 - i) * 4)) & 0xF];
    out[10] = '\0';
}

/* Convert a non-negative integer to IEEE-754 float bits (exact for |v| < 2^24). */
static uint32_t u32_to_f32bits(uint32_t a)
{
    if (a == 0) return 0;
    int msb = 31;
    while (!(a & (1u << msb))) msb--;
    uint32_t mant = (msb >= 23) ? ((a >> (msb - 23)) & 0x7FFFFFu)
                                : ((a << (23 - msb)) & 0x7FFFFFu);
    uint32_t exp = (uint32_t)(msb + 127);
    return (exp << 23) | mant;
}

int patch_parse_value(const char *tok, uint32_t *bits)
{
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        uint32_t v = 0;
        int i = 2;
        if (!tok[i]) return PATCH_EFMT;
        for (; tok[i]; i++) {
            char c = tok[i];
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return PATCH_EFMT;
            v = (v << 4) | d;
        }
        *bits = v;
        return PATCH_OK;
    }

    /* Signed decimal integer -> float bits. */
    int neg = 0, i = 0;
    if (tok[0] == '-') { neg = 1; i = 1; }
    else if (tok[0] == '+') { i = 1; }
    if (!tok[i]) return PATCH_EFMT;
    uint32_t a = 0;
    for (; tok[i]; i++) {
        if (tok[i] < '0' || tok[i] > '9') return PATCH_EFMT;
        a = a * 10u + (uint32_t)(tok[i] - '0');
    }
    uint32_t b = u32_to_f32bits(a);
    if (neg && b) b |= 0x80000000u;
    *bits = b;
    return PATCH_OK;
}

/* ---- model builders ------------------------------------------------------ */

void patch_init(patch_t *p)
{
    p->n_plugins = 0;
    p->n_params  = 0;
    p->n_edges   = 0;
}

int patch_add_plugin(patch_t *p, const char *path)
{
    if (p->n_plugins >= PATCH_MAX_PLUGINS) return PATCH_ERANGE;
    int i = 0;
    for (; path[i] && i < PATCH_PATH_MAX - 1; i++)
        p->plugins[p->n_plugins].path[i] = path[i];
    if (path[i]) return PATCH_ERANGE;             /* path too long */
    p->plugins[p->n_plugins].path[i] = '\0';
    return p->n_plugins++;
}

int patch_add_param(patch_t *p, int plugin, uint32_t id, uint32_t bits)
{
    if (p->n_params >= PATCH_MAX_PARAMS) return PATCH_ERANGE;
    if (plugin < 0 || plugin >= p->n_plugins) return PATCH_ERANGE;
    p->params[p->n_params].plugin = plugin;
    p->params[p->n_params].id     = id;
    p->params[p->n_params].bits   = bits;
    return p->n_params++;
}

int patch_add_edge(patch_t *p, int src, int dst)
{
    if (p->n_edges >= PATCH_MAX_EDGES) return PATCH_ERANGE;
    if (src != PATCH_INPUT && (src < 0 || src >= p->n_plugins)) return PATCH_ERANGE;
    if (dst != PATCH_DAC && (dst < 0 || dst >= p->n_plugins)) return PATCH_ERANGE;
    p->edges[p->n_edges].src = src;
    p->edges[p->n_edges].dst = dst;
    return p->n_edges++;
}

/* ---- serialise ----------------------------------------------------------- */

long patch_serialize(const patch_t *p, char *out, uint32_t max)
{
    uint32_t pos = 0;
    char val[11];

    if (put(out, &pos, max, "# tessera-patch v1\n")) return PATCH_ENOSPACE;

    for (int i = 0; i < p->n_plugins; i++)
        if (put(out, &pos, max, "plugin ") ||
            put(out, &pos, max, p->plugins[i].path) ||
            put(out, &pos, max, "\n"))
            return PATCH_ENOSPACE;

    for (int i = 0; i < p->n_params; i++) {
        patch_format_value(p->params[i].bits, val);
        if (put(out, &pos, max, "param ") ||
            put_uint(out, &pos, max, (uint32_t)p->params[i].plugin) ||
            put(out, &pos, max, " ") ||
            put_uint(out, &pos, max, p->params[i].id) ||
            put(out, &pos, max, " ") ||
            put(out, &pos, max, val) ||
            put(out, &pos, max, "\n"))
            return PATCH_ENOSPACE;
    }

    for (int i = 0; i < p->n_edges; i++) {
        if (put(out, &pos, max, "connect ")) return PATCH_ENOSPACE;
        if (p->edges[i].src == PATCH_INPUT) {
            if (put(out, &pos, max, "input")) return PATCH_ENOSPACE;
        } else if (put_uint(out, &pos, max, (uint32_t)p->edges[i].src)) {
            return PATCH_ENOSPACE;
        }
        if (put(out, &pos, max, " ")) return PATCH_ENOSPACE;
        if (p->edges[i].dst == PATCH_DAC) {
            if (put(out, &pos, max, "dac")) return PATCH_ENOSPACE;
        } else if (put_uint(out, &pos, max, (uint32_t)p->edges[i].dst)) {
            return PATCH_ENOSPACE;
        }
        if (put(out, &pos, max, "\n")) return PATCH_ENOSPACE;
    }

    out[pos] = '\0';
    return (long)pos;
}

/* ---- parse --------------------------------------------------------------- */

/* Read one whitespace-delimited token from in[*i..end) into tok (cap n).
 * Returns the token length, or 0 at end-of-line/input. */
static int token(const char *in, uint32_t *i, uint32_t end, char *tok, int n)
{
    while (*i < end && (in[*i] == ' ' || in[*i] == '\t')) (*i)++;
    int k = 0;
    while (*i < end && in[*i] != ' ' && in[*i] != '\t' &&
           in[*i] != '\n' && in[*i] != '\r') {
        if (k < n - 1) tok[k++] = in[*i];
        (*i)++;
    }
    tok[k] = '\0';
    return k;
}

static void skip_to_eol(const char *in, uint32_t *i, uint32_t end)
{
    while (*i < end && in[*i] != '\n') (*i)++;
    if (*i < end) (*i)++;                 /* consume the newline */
}

static int parse_uint(const char *tok, uint32_t *out)
{
    if (!tok[0]) return PATCH_EFMT;
    uint32_t v = 0;
    for (int i = 0; tok[i]; i++) {
        if (tok[i] < '0' || tok[i] > '9') return PATCH_EFMT;
        v = v * 10u + (uint32_t)(tok[i] - '0');
    }
    *out = v;
    return PATCH_OK;
}

int patch_parse(const char *in, uint32_t len, patch_t *p)
{
    patch_init(p);
    uint32_t i = 0;
    char kw[16], a[PATCH_PATH_MAX], b[16], c[16];

    while (i < len) {
        /* Skip blank space at line start; peek for comments/blank lines. */
        uint32_t save = i;
        int kn = token(in, &i, len, kw, sizeof(kw));
        if (kn == 0) {                    /* blank line */
            if (i == save) skip_to_eol(in, &i, len);
            else skip_to_eol(in, &i, len);
            continue;
        }
        if (kw[0] == '#') { skip_to_eol(in, &i, len); continue; }

        if (streq(kw, "plugin")) {
            if (token(in, &i, len, a, sizeof(a)) == 0) return PATCH_ETRUNC;
            if (patch_add_plugin(p, a) < 0) return PATCH_ERANGE;
        } else if (streq(kw, "param")) {
            if (token(in, &i, len, a, sizeof(a)) == 0) return PATCH_ETRUNC;
            if (token(in, &i, len, b, sizeof(b)) == 0) return PATCH_ETRUNC;
            if (token(in, &i, len, c, sizeof(c)) == 0) return PATCH_ETRUNC;
            uint32_t pi, id, bits;
            if (parse_uint(a, &pi) != PATCH_OK) return PATCH_EFMT;
            if (parse_uint(b, &id) != PATCH_OK) return PATCH_EFMT;
            if (patch_parse_value(c, &bits) != PATCH_OK) return PATCH_EFMT;
            if (patch_add_param(p, (int)pi, id, bits) < 0) return PATCH_ERANGE;
        } else if (streq(kw, "connect")) {
            if (token(in, &i, len, a, sizeof(a)) == 0) return PATCH_ETRUNC;
            if (token(in, &i, len, b, sizeof(b)) == 0) return PATCH_ETRUNC;
            int si, di;
            if (streq(a, "input")) {
                si = PATCH_INPUT;
            } else {
                uint32_t s;
                if (parse_uint(a, &s) != PATCH_OK) return PATCH_EFMT;
                si = (int)s;
            }
            if (streq(b, "dac")) {
                di = PATCH_DAC;
            } else {
                uint32_t d;
                if (parse_uint(b, &d) != PATCH_OK) return PATCH_EFMT;
                di = (int)d;
            }
            if (patch_add_edge(p, si, di) < 0) return PATCH_ERANGE;
        } else {
            return PATCH_EFMT;            /* unknown keyword */
        }
        skip_to_eol(in, &i, len);
    }
    return PATCH_OK;
}
