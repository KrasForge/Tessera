/* arch/arm64/patch.h - preset/patch file format (Issue #40, M9)
 *
 * A patch captures a graph configuration so it can be saved to storage and
 * reloaded: which plugins are loaded (by ELF path), how they are wired, and a
 * flat list of {param_id, value} per plugin.  The on-disk form is a small
 * line-based text format that a developer can read and edit:
 *
 *     # tessera-patch v1
 *     plugin /sd/synth.elf          # plugin 0
 *     plugin /sd/effect.elf         # plugin 1
 *     param 0 0 0x43dc0000          # plugin 0, param 0 = 440.0 (IEEE-754 hex)
 *     param 1 2 0x3f000000          # plugin 1, param 2 = 0.5
 *     connect 0 1                   # plugin 0 -> plugin 1
 *     connect 1 dac                 # plugin 1 -> DAC sink
 *
 * Parameter values are 32-bit IEEE-754 hex bit patterns (exact, and the kernel
 * needs no floating point to read or write them).  When editing by hand a plain
 * decimal integer is also accepted (e.g. `param 0 0 440`).
 *
 * This module is pure text <-> model and is unit-tested on the host
 * (make test-arm-patch); the kernel glue that captures live state and applies a
 * parsed patch lives in patch_mgr.c.
 */

#ifndef ARM64_PATCH_H
#define ARM64_PATCH_H

#include <stdint.h>
#include <stddef.h>

#define PATCH_MAX_PLUGINS 16
#define PATCH_MAX_PARAMS  128
#define PATCH_MAX_EDGES   32
#define PATCH_PATH_MAX    64

#define PATCH_DAC   (-1)   /* edge destination: the DAC sink (pid 0)        */
#define PATCH_INPUT (-2)   /* edge source: the capture input (issue #84)    */

typedef struct { char path[PATCH_PATH_MAX]; } patch_plugin_t;
typedef struct { int plugin; uint32_t id; uint32_t bits; } patch_param_t;
/* src may be a plugin index or PATCH_INPUT; dst a plugin index or PATCH_DAC. */
typedef struct { int src; int dst; } patch_edge_t;

typedef struct {
    patch_plugin_t plugins[PATCH_MAX_PLUGINS];
    int            n_plugins;
    patch_param_t  params[PATCH_MAX_PARAMS];
    int            n_params;
    patch_edge_t   edges[PATCH_MAX_EDGES];
    int            n_edges;
} patch_t;

/* Errors (negative). */
#define PATCH_OK        0
#define PATCH_ETRUNC  (-1)   /* input ended mid-record (truncated file) */
#define PATCH_EFMT    (-2)   /* malformed token / unknown keyword       */
#define PATCH_ERANGE  (-3)   /* index out of range / table full         */
#define PATCH_ENOSPACE (-4)  /* serialize output buffer too small       */

void patch_init(patch_t *p);

/* Build a model.  Return the new plugin index, or a negative PATCH_E* */
int patch_add_plugin(patch_t *p, const char *path);
int patch_add_param(patch_t *p, int plugin, uint32_t id, uint32_t bits);
int patch_add_edge(patch_t *p, int src, int dst);   /* dst PATCH_DAC ok */

/* Serialise `p` to text in `out` (capacity `max`).  Returns the byte length
 * written (excluding the NUL), or PATCH_ENOSPACE. */
long patch_serialize(const patch_t *p, char *out, uint32_t max);

/* Parse `len` bytes of patch text into `p`.  Returns PATCH_OK, or a negative
 * PATCH_E* on malformed or truncated input (never reads out of bounds). */
int patch_parse(const char *in, uint32_t len, patch_t *p);

/* Value codec (no floating point).  Format writes "0x%08x"; parse accepts a
 * 0x-prefixed 32-bit hex value or a signed decimal integer. */
void patch_format_value(uint32_t bits, char out[11]);
int  patch_parse_value(const char *tok, uint32_t *bits);

#endif /* ARM64_PATCH_H */
