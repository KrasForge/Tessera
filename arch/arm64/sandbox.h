/* arch/arm64/sandbox.h - plugin sandbox audit (Issue #35, M8)
 *
 * Loading a plugin into its own address space (#34) is necessary but not
 * sufficient: the sandbox must be provably airtight.  sandbox_audit() walks a
 * process's user page tables and asserts that every present mapping falls
 * inside an explicit allowlist of regions (the plugin's own code/rodata/data/
 * BSS/stack plus the shared audio buffer) and that none is a device (MMIO)
 * mapping or simultaneously writable and executable at EL0.  Anything else is
 * an unexpected mapping and a sandbox breach.
 *
 * The runtime half of the sandbox - that an SVC from inside the plugin body is
 * fatal - lives in the syscall path (process_set_svc_gate(), issue #35); this
 * module is the static audit of the address space.
 */

#ifndef ARM64_SANDBOX_H
#define ARM64_SANDBOX_H

#include "process.h"
#include <stdint.h>

/* A permitted [va, va+len) window in the plugin address space (page-granular). */
typedef struct sandbox_region {
    uint64_t va;
    uint64_t len;
} sandbox_region_t;

typedef struct {
    int      total_pages;   /* present user pages found in the page tables   */
    int      outside_pages; /* pages whose VA is in no allowed region        */
    int      device_pages;  /* pages mapped as device / MMIO memory          */
    int      wx_pages;      /* pages both writable and executable at EL0      */
    uint64_t first_bad_va;  /* first offending VA, or 0 if the audit is clean */
} sandbox_report_t;

/* Walk p's user-space page tables and classify every present 4 KiB leaf
 * against `allow` (n_allow windows).  Fills `*r` (may be NULL).  Returns 0 if
 * the sandbox is airtight, or a negative count of violations otherwise. */
int sandbox_audit(const process_t *p, const sandbox_region_t *allow,
                  int n_allow, sandbox_report_t *r);

/* ---- pure helpers (host-testable, see tests/arm64/sandbox_test.c) ---- */

/* Is `va` inside any of the `n` allowed windows? */
int sandbox_va_allowed(uint64_t va, const sandbox_region_t *allow, int n);

/* Decode a leaf page descriptor's EL0-relevant attributes: sets *device if it
 * maps device/MMIO memory and *wx if it is both writable and executable at
 * EL0 (a W^X violation). */
void sandbox_classify_pte(uint64_t pte, int *device, int *wx);

#endif /* ARM64_SANDBOX_H */
