/* arch/arm64/denorm.h - denormal (subnormal) protection (Theme H, issue #130)
 *
 * Subnormal floats cost 10-100x a normal op on many FPUs, so a decaying reverb
 * tail or filter state drifting into the subnormal range can blow a plugin's CPU
 * budget for no audible reason.  The platform guarantee: flush-to-zero is on
 * whenever audio DSP runs.
 *
 * On AArch64 that is FPCR.FZ (bit 24), which flushes both subnormal *inputs* and
 * *outputs* to zero (the FTZ + DAZ behaviour) for single/double ops.  The kernel
 * seeds every task's saved FPCR with FZ set (sched.c), so a plugin inherits
 * flush-to-zero the moment its FP context is restored - it cannot forget to
 * enable it.
 *
 * This module is integer-only (it manipulates the FPCR value and float *bit
 * patterns*, never float types), so it compiles into the -mgeneral-regs-only
 * kernel.  Pure, host-tested (make test-arm-denorm).
 */

#ifndef ARM64_DENORM_H
#define ARM64_DENORM_H

#include <stdint.h>

/* FPCR flush-to-zero bit (Armv8-A): flush subnormal inputs and results to zero. */
#define FPCR_FZ (1u << 24)

/* Return `fpcr` with flush-to-zero enabled. */
static inline uint32_t denorm_fpcr_set_ftz(uint32_t fpcr) { return fpcr | FPCR_FZ; }

/* Whether flush-to-zero is enabled in `fpcr`. */
static inline int denorm_fpcr_ftz_enabled(uint32_t fpcr) { return (fpcr & FPCR_FZ) != 0; }

/* The default FPCR the kernel installs for a fresh task (flush-to-zero on). */
static inline uint32_t denorm_fpcr_default(void) { return FPCR_FZ; }

/* ---- software fallback, operating on 32-bit float bit patterns ------------ *
 * For portability and for reference/tests: detect and flush subnormals without
 * touching a float register (so it is safe in the integer-only kernel too). */

/* Is `bits` (an IEEE-754 binary32 bit pattern) a subnormal (exponent 0, non-zero
 * mantissa)?  Zero is NOT subnormal. */
static inline int denorm_is_subnormal(uint32_t bits)
{
    return ((bits & 0x7f800000u) == 0u) && ((bits & 0x007fffffu) != 0u);
}

/* Flush a subnormal bit pattern to a correctly-signed zero; other values pass
 * through unchanged. */
static inline uint32_t denorm_flush(uint32_t bits)
{
    return denorm_is_subnormal(bits) ? (bits & 0x80000000u) : bits;
}

#endif /* ARM64_DENORM_H */
