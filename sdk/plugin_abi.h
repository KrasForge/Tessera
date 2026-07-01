/* include/plugin_abi.h - Tessera plugin binary interface (Issue #23, M5)
 *
 * This is the contract between a plugin author and the Tessera host.  A plugin
 * is an AArch64 binary that exports the five C entry points below; the host
 * resolves them by name, checks plugin_abi_version() for compatibility, then
 * drives the plugin from the real-time audio thread.
 *
 * The interface is plain C (AAPCS64 calling convention, no name mangling, no
 * C++ types) and this header is intentionally SELF-CONTAINED: it includes only
 * <stdint.h>, so a third party can build a plugin against this one header with
 * a stock aarch64 toolchain and no Tessera kernel sources.
 *
 * ----------------------------------------------------------------------------
 * Real-time contract
 * ----------------------------------------------------------------------------
 * plugin_process_block() runs on the dedicated audio core (issue #21) inside
 * the audio callback.  It MUST be real-time-safe:
 *
 *   - No memory allocation (no malloc/free/new/delete).
 *   - No blocking: no locks that another thread can hold, no waiting on I/O.
 *   - No system calls of any kind.
 *   - No unbounded loops; work must be O(n_frames) and complete well within
 *     one block period (block_size / sample_rate seconds).
 *
 * plugin_set_param() is also called from the audio path and must be
 * real-time-safe and wait-free (a single store, or a lock-free publish).
 *
 * plugin_init() and plugin_destroy() are the only entry points that may
 * allocate or perform non-real-time work; they run at load/unload time on a
 * general-purpose core, never from the audio callback.
 */

#ifndef TESSERA_PLUGIN_ABI_H
#define TESSERA_PLUGIN_ABI_H

/* ===========================================================================
 * STABILITY: this interface is FROZEN as Tessera Plugin ABI v1.0 (issue #37).
 *
 * The five exported symbols below, their signatures, the AAPCS64 calling
 * convention, and the versioning rules are a stable contract: they will not
 * change without a MAJOR version bump (TESSERA_PLUGIN_ABI_VERSION_MAJOR).
 * Backward-compatible additions bump only the MINOR version.  The full,
 * self-contained specification a third party needs to build a plugin lives in
 * docs/plugin-abi.md; this header is its normative source.
 * ========================================================================= */

#define TESSERA_PLUGIN_ABI_STABLE 1   /* v1 frozen; see docs/plugin-abi.md */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version: major in the high 16 bits, minor in the low 16 bits.  The host
 * accepts a plugin only when its major matches and its minor is <= the host's
 * minor, so additive (minor) changes stay backward compatible. */
#define TESSERA_PLUGIN_ABI_VERSION_MAJOR 1u
#define TESSERA_PLUGIN_ABI_VERSION_MINOR 0u
#define TESSERA_PLUGIN_ABI_VERSION \
    ((TESSERA_PLUGIN_ABI_VERSION_MAJOR << 16) | TESSERA_PLUGIN_ABI_VERSION_MINOR)

/* Freeze guard: the v1 line is MAJOR 1.  A change here is an intentional,
 * reviewed ABI break, not an accident. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(TESSERA_PLUGIN_ABI_VERSION_MAJOR == 1u,
               "Tessera Plugin ABI v1 is frozen: bumping MAJOR is an ABI break");
#endif

/* Return codes for plugin_init(). */
#define TESSERA_PLUGIN_OK        0
#define TESSERA_PLUGIN_EVERSION (-1)   /* unsupported sample rate / block size */
#define TESSERA_PLUGIN_ENOMEM   (-2)   /* could not allocate plugin state      */
#define TESSERA_PLUGIN_EINVAL   (-3)   /* invalid argument                     */

/* ----------------------------------------------------------------------------
 * Required entry points (a plugin MUST export all of these symbols)
 * ------------------------------------------------------------------------- */

/* Compatibility handshake.  Returns the ABI version the plugin was built
 * against (TESSERA_PLUGIN_ABI_VERSION).  The host calls this first and rejects
 * the plugin if the major version does not match.  Must be callable before
 * plugin_init(). */
uint32_t plugin_abi_version(void);

/* One-time setup at load.  `sample_rate` (Hz) and `block_size` (frames per
 * process_block call) are fixed for the plugin's lifetime.  Returns
 * TESSERA_PLUGIN_OK on success or a negative TESSERA_PLUGIN_E* code.  May
 * allocate; runs off the audio path. */
int plugin_init(uint32_t sample_rate, uint32_t block_size);

/* Process one block of de-interleaved stereo audio.  `in_l`/`in_r` are the
 * left/right input buffers and `out_l`/`out_r` the outputs, each `n_frames`
 * floats.  In-place is not assumed; out may alias in only if the plugin
 * handles it.  REAL-TIME SAFE: no allocation, no locks, no syscalls (see the
 * real-time contract above). */
void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames);

/* Update a parameter by id.  Real-time-safe and wait-free; called from the
 * audio path.  Unknown `param_id`s must be ignored. */
void plugin_set_param(uint32_t param_id, float value);

/* Release any resources acquired in plugin_init().  Called once before unload,
 * off the audio path.  After this returns no other entry point will be
 * called. */
void plugin_destroy(void);

/* ----------------------------------------------------------------------------
 * Function-pointer typedefs (for the host's symbol table / dispatch)
 * ------------------------------------------------------------------------- */
typedef uint32_t (*plugin_abi_version_fn)(void);
typedef int      (*plugin_init_fn)(uint32_t sample_rate, uint32_t block_size);
typedef void     (*plugin_process_block_fn)(const float *in_l, const float *in_r,
                                            float *out_l, float *out_r,
                                            uint32_t n_frames);
typedef void     (*plugin_set_param_fn)(uint32_t param_id, float value);
typedef void     (*plugin_destroy_fn)(void);

/* The host fills one of these by resolving the five symbols above by name.
 * The symbol names are the canonical ABI; this struct is a convenience for the
 * loader and is not itself part of the binary contract. */
typedef struct {
    plugin_abi_version_fn   abi_version;
    plugin_init_fn          init;
    plugin_process_block_fn process_block;
    plugin_set_param_fn     set_param;
    plugin_destroy_fn       destroy;
} tessera_plugin_t;

/* Canonical symbol names the host resolves. */
#define TESSERA_PLUGIN_SYM_ABI_VERSION   "plugin_abi_version"
#define TESSERA_PLUGIN_SYM_INIT          "plugin_init"
#define TESSERA_PLUGIN_SYM_PROCESS_BLOCK "plugin_process_block"
#define TESSERA_PLUGIN_SYM_SET_PARAM     "plugin_set_param"
#define TESSERA_PLUGIN_SYM_DESTROY       "plugin_destroy"

#ifdef __cplusplus
}
#endif

#endif /* TESSERA_PLUGIN_ABI_H */
