/* arch/arm64/presets.h - embedded plugin presets + sample-rate/block-size
 * negotiation (Theme F, issue #127)
 *
 * Two related pieces of plugin self-description that the host reads before or at
 * load:
 *
 *   1. Embedded presets.  A plugin ships factory presets inside its ELF (a
 *      `.tessera.presets` section): named snapshots of {param_id, value}
 *      settings.  The host reads the section blob and can apply a preset without
 *      the author shipping a separate file.  This module parses the blob.
 *
 *   2. Config negotiation.  A plugin advertises the sample rates and block sizes
 *      it supports (a `plugin_caps_t`).  The host offers its rate/block; if the
 *      plugin accepts them they are used directly, otherwise negotiation
 *      suggests the plugin's nearest workable config so the host can resample /
 *      re-buffer or reject.
 *
 * The preset blob comes from an untrusted plugin file, so every field is
 * bounds-checked against the buffer.  Values are stored as 32-bit IEEE-754 bit
 * patterns (like the patch format), so the kernel needs no FP.  Pure,
 * host-tested (make test-arm-presets).
 */

#ifndef ARM64_PRESETS_H
#define ARM64_PRESETS_H

#include <stdint.h>

/* ---- embedded presets ---------------------------------------------------- *
 * Blob layout (little-endian):
 *   0  u32 magic 'TPRE'
 *   4  u16 version (1)
 *   6  u16 n_presets
 *   8  preset[0], preset[1], ...
 * each preset:
 *   char name[24]   (NUL-padded)
 *   u16  n_params
 *   u16  reserved
 *   then n_params * { u32 param_id, u32 value_bits }
 */
#define PRESET_MAGIC     0x45525054u   /* 'TPRE' little-endian */
#define PRESET_VERSION   1u
#define PRESET_NAME_LEN  24
#define PRESET_SECTION   ".tessera.presets"

typedef struct { uint32_t id; uint32_t bits; } preset_param_t;

typedef struct {
    const uint8_t *blob;
    uint32_t       len;
    uint16_t       n_presets;
} preset_table_t;

typedef struct {
    char            name[PRESET_NAME_LEN + 1];
    uint16_t        n_params;
    const uint8_t  *params;    /* raw {id,bits} pairs, big enough for n_params */
} preset_info_t;

/* Validate a preset-section blob and open it for reading.  Returns 0 on success
 * (filling `t`), -1 if the blob is malformed / truncated. */
int  presets_open(preset_table_t *t, const uint8_t *blob, uint32_t len);

/* Number of presets in an opened table. */
int  presets_count(const preset_table_t *t);

/* Read preset `index` (0-based): its name and parameter count/pointer.  Returns
 * 0 on success, -1 on a bad index or a preset that runs past the blob. */
int  presets_get(const preset_table_t *t, int index, preset_info_t *out);

/* Read parameter `i` of a preset into `*id`/`*bits`.  Returns 0 or -1. */
int  preset_param(const preset_info_t *p, int i, uint32_t *id, uint32_t *bits);

/* Serialize helper (build/provisioning + tests): append one preset to a blob
 * being assembled in `buf`.  `off` is the current length (start at 0 with the
 * header written by presets_build_header).  Returns the new length or -1. */
int  presets_build_header(uint8_t *buf, int cap, uint16_t n_presets);
int  presets_build_add(uint8_t *buf, int cap, int off, const char *name,
                       const preset_param_t *params, uint16_t n_params);

/* ---- config negotiation -------------------------------------------------- */

#define PLUGIN_CAPS_MAX_RATES 8

typedef struct {
    uint32_t rates[PLUGIN_CAPS_MAX_RATES];  /* supported sample rates (Hz) */
    int      n_rates;                        /* 0 = accepts any rate        */
    uint32_t block_min, block_max;           /* inclusive; 0/0 = any block  */
} plugin_caps_t;

/* Does the plugin accept this sample rate / block size? */
int caps_supports_rate (const plugin_caps_t *c, uint32_t sr);
int caps_supports_block(const plugin_caps_t *c, uint32_t block);

/* Negotiate a config.  If (host_sr, host_block) is directly acceptable, writes
 * them to out_sr / out_block and returns 1.  Otherwise writes the plugin's
 * nearest workable config (closest supported rate; host_block clamped to
 * [block_min, block_max]) and returns 0, so the host can resample/re-buffer or
 * reject.  Returns -1 only if the plugin advertises no workable config at all. */
int caps_negotiate(const plugin_caps_t *c, uint32_t host_sr, uint32_t host_block,
                   uint32_t *out_sr, uint32_t *out_block);

#endif /* ARM64_PRESETS_H */
