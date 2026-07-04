/* arch/arm64/presets.c - embedded plugin presets + config negotiation
 * (Theme F, issue #127).  See presets.h. */

#include "presets.h"

#define PRESET_HDR_LEN    8    /* magic(4) + version(2) + n_presets(2) */
#define PRESET_ENTRY_HDR  (PRESET_NAME_LEN + 4)   /* name + n_params(2) + rsvd(2) */

static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

/* ---- reading ------------------------------------------------------------- */

int presets_open(preset_table_t *t, const uint8_t *blob, uint32_t len)
{
    if (!blob || len < PRESET_HDR_LEN)
        return -1;
    if (get_u32(blob) != PRESET_MAGIC)
        return -1;
    if (get_u16(blob + 4) != PRESET_VERSION)
        return -1;
    t->blob      = blob;
    t->len       = len;
    t->n_presets = get_u16(blob + 6);
    return 0;
}

int presets_count(const preset_table_t *t) { return (int)t->n_presets; }

int presets_get(const preset_table_t *t, int index, preset_info_t *out)
{
    if (index < 0 || index >= (int)t->n_presets)
        return -1;

    /* Walk the variable-length entries from the start, bounds-checking each so a
     * malformed blob cannot walk off the end. */
    uint32_t off = PRESET_HDR_LEN;
    for (int i = 0; i <= index; i++) {
        if (off + PRESET_ENTRY_HDR > t->len)
            return -1;
        uint16_t n_params = get_u16(t->blob + off + PRESET_NAME_LEN);
        uint32_t body = (uint32_t)n_params * 8u;         /* {id,bits} pairs */
        /* Guard the multiply/add against overflow before the range check. */
        if (n_params > (t->len / 8u) + 1u)
            return -1;
        uint32_t entry = PRESET_ENTRY_HDR + body;
        if (off + entry > t->len)
            return -1;

        if (i == index) {
            for (int c = 0; c < PRESET_NAME_LEN; c++)
                out->name[c] = (char)t->blob[off + c];
            out->name[PRESET_NAME_LEN] = '\0';
            out->n_params = n_params;
            out->params   = t->blob + off + PRESET_ENTRY_HDR;
            return 0;
        }
        off += entry;
    }
    return -1;
}

int preset_param(const preset_info_t *p, int i, uint32_t *id, uint32_t *bits)
{
    if (i < 0 || i >= (int)p->n_params)
        return -1;
    const uint8_t *e = p->params + (uint32_t)i * 8u;
    if (id)   *id   = get_u32(e);
    if (bits) *bits = get_u32(e + 4);
    return 0;
}

/* ---- building ------------------------------------------------------------ */

int presets_build_header(uint8_t *buf, int cap, uint16_t n_presets)
{
    if (cap < PRESET_HDR_LEN)
        return -1;
    put_u32(buf + 0, PRESET_MAGIC);
    put_u16(buf + 4, PRESET_VERSION);
    put_u16(buf + 6, n_presets);
    return PRESET_HDR_LEN;
}

int presets_build_add(uint8_t *buf, int cap, int off, const char *name,
                      const preset_param_t *params, uint16_t n_params)
{
    if (off < 0)
        return -1;
    int64_t entry = (int64_t)PRESET_ENTRY_HDR + (int64_t)n_params * 8;
    if ((int64_t)off + entry > cap)
        return -1;

    int done = 0;
    for (int i = 0; i < PRESET_NAME_LEN; i++) {
        if (!done && name && name[i]) buf[off + i] = (uint8_t)name[i];
        else { buf[off + i] = 0; done = 1; }
    }
    put_u16(buf + off + PRESET_NAME_LEN, n_params);
    put_u16(buf + off + PRESET_NAME_LEN + 2, 0);       /* reserved */
    int p = off + PRESET_ENTRY_HDR;
    for (int i = 0; i < n_params; i++) {
        put_u32(buf + p,     params[i].id);
        put_u32(buf + p + 4, params[i].bits);
        p += 8;
    }
    return p;
}

/* ---- negotiation --------------------------------------------------------- */

int caps_supports_rate(const plugin_caps_t *c, uint32_t sr)
{
    if (c->n_rates <= 0)
        return 1;                          /* accepts any rate */
    for (int i = 0; i < c->n_rates && i < PLUGIN_CAPS_MAX_RATES; i++)
        if (c->rates[i] == sr)
            return 1;
    return 0;
}

int caps_supports_block(const plugin_caps_t *c, uint32_t block)
{
    if (c->block_min == 0 && c->block_max == 0)
        return 1;                          /* any block size */
    if (c->block_min && block < c->block_min) return 0;
    if (c->block_max && block > c->block_max) return 0;
    return 1;
}

static uint32_t abs_diff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

int caps_negotiate(const plugin_caps_t *c, uint32_t host_sr, uint32_t host_block,
                   uint32_t *out_sr, uint32_t *out_block)
{
    /* Choose a rate: the host's if supported, else the nearest advertised one. */
    uint32_t sr;
    if (caps_supports_rate(c, host_sr)) {
        sr = host_sr;
    } else if (c->n_rates > 0) {
        sr = c->rates[0];
        uint32_t best = abs_diff(sr, host_sr);
        for (int i = 1; i < c->n_rates && i < PLUGIN_CAPS_MAX_RATES; i++) {
            uint32_t d = abs_diff(c->rates[i], host_sr);
            if (d < best) { best = d; sr = c->rates[i]; }
        }
    } else {
        return -1;                          /* no advertised rate (n_rates<0?) */
    }

    /* Clamp the block size into the plugin's range. */
    uint32_t block = host_block;
    if (c->block_min && block < c->block_min) block = c->block_min;
    if (c->block_max && block > c->block_max) block = c->block_max;

    if (out_sr)    *out_sr    = sr;
    if (out_block) *out_block = block;

    return (sr == host_sr && block == host_block) ? 1 : 0;
}
