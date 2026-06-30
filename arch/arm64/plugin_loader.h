/* arch/arm64/plugin_loader.h - load a plugin ELF into an isolated address
 *                              space (Issue #24, M5)
 *
 * The ARM port of the IKOS ELF/app loader (kernel/elf_loader.c,
 * kernel/app_loader.c).  Creates a fresh per-plugin address space with
 * process_create() (issue #11), maps the ELF's PT_LOAD segments into it with
 * MMU-enforced permissions, resolves the plugin ABI symbols (issue #23), and
 * enters plugin_init at EL0 through a controlled trampoline.  A plugin can
 * therefore reach only its own segments, its stack, and the host-mapped
 * parameter page; any access to kernel memory faults and the plugin is killed
 * (issue #14).
 */

#ifndef ARM64_PLUGIN_LOADER_H
#define ARM64_PLUGIN_LOADER_H

#include "process.h"
#include <stdint.h>
#include <stddef.h>

struct sandbox_region;   /* arch/arm64/sandbox.h */

/* Fixed VAs in the plugin's address space, placed well above the (tiny) ELF
 * segments at USER_VA_BASE so they never collide. */
#define PLUGIN_STACK_VA  (USER_VA_BASE + 0x08000000ull)
#define PLUGIN_TRAMP_VA  (USER_VA_BASE + 0x09000000ull)
#define PLUGIN_PARAM_VA  (USER_VA_BASE + 0x0A000000ull)

/* A mapped region of the plugin's address space, recorded as the loader maps
 * it so the sandbox audit (issue #35) has an exact allowlist of what should be
 * present - and nothing else. */
#define PLUGIN_MAX_REGIONS 16
typedef struct {
    uint64_t va;
    uint64_t len;
    unsigned flags;          /* VMM_* the region was mapped with */
} plugin_region_t;

typedef struct {
    process_t *proc;
    uint64_t   abi_version_va;
    uint64_t   init_va;
    uint64_t   process_va;
    uint64_t   setparam_va;
    uint64_t   destroy_va;
    uint64_t   entry_va;     /* trampoline VA (EL0 entry)       */
    uint64_t   stack_top;
    uint64_t   param_va;
    uintptr_t  param_pa;     /* kernel-visible alias of the param page */
    plugin_region_t regions[PLUGIN_MAX_REGIONS];
    int        n_regions;
} plugin_t;

/* Errors. */
#define PLUGIN_OK         0
#define PLUGIN_EBADELF  (-1)
#define PLUGIN_ENOPROC  (-2)
#define PLUGIN_ENOMEM   (-3)
#define PLUGIN_ENOSYM   (-4)   /* required ABI symbol (plugin_init) missing */

/* Load the plugin ELF at `elf`/`len` into a new isolated address space.
 * Returns PLUGIN_OK and fills `pl`, or a negative PLUGIN_E* code. */
int plugin_load(plugin_t *pl, const void *elf, size_t len, const char *name);

/* Enter plugin_init(sample_rate, block_size) at EL0 via the trampoline.
 * Returns the plugin's init return code, or -1 if the plugin faulted and was
 * killed by the MMU. */
long plugin_call_init(plugin_t *pl, uint32_t sample_rate, uint32_t block_size);

/* Run the plugin's plugin_abi_version() handshake at EL0 (the safe, designated
 * first call) and return the version it reports, or -1 if the symbol is missing
 * or the call faulted.  Used to validate the ABI before plugin_init runs. */
long plugin_call_abi_version(plugin_t *pl);

/* Map an existing physical region [pa, pa+bytes) into the plugin's address
 * space at `va` with `flags` (VMM_*).  Used to share the audio ring buffer
 * (issue #25) into the plugin at a fixed VA without copying.  Returns 0 on
 * success. */
int plugin_map_region(plugin_t *pl, uint64_t va, uintptr_t pa, size_t bytes,
                      unsigned flags);

/* Copy the plugin's recorded mapped regions into `out` (capacity `max`) as
 * page-granular [va, len) windows for sandbox_audit().  Returns the count. */
int plugin_sandbox_regions(const plugin_t *pl, struct sandbox_region *out,
                           int max);

#endif /* ARM64_PLUGIN_LOADER_H */
