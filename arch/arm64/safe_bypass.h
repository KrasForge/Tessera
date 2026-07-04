/* arch/arm64/safe_bypass.h - never-go-silent safe-mode bypass
 *                            (Theme A: reliability)
 *
 * The payoff of the isolation architecture.  When an effect in the signal path
 * suffers a fatal fault (an MMU data abort, a forbidden syscall, or a budget
 * kill - all already caught by M8/M12), the node stops producing output and,
 * without this, the DAC downstream of it would go silent: a pedal dead on
 * stage.  Safe-mode bypass detects the dead node and routes its *input*
 * straight to its consumer - a clean bypass - so the audio path heals to
 * clean-through and never stops.
 *
 * This is a single-process (Linux/Elk) audio host cannot safely offer: there a
 * plugin fault takes down the whole engine.  Here the fault is contained and
 * the platform substitutes the dry signal.
 *
 * The logic is pure and FP-free (samples are copied as raw 32-bit words, so it
 * runs on the -mgeneral-regs-only audio path) and is unit-tested on the host
 * (make test-arm-safe-bypass) and demonstrated end to end on QEMU virt
 * (make test-arm-safe-bypass-qemu).
 */

#ifndef ARM64_SAFE_BYPASS_H
#define ARM64_SAFE_BYPASS_H

#include <stdint.h>

/* Bypass state for one effect slot in the signal path. */
typedef struct {
    uint32_t bypassed;        /* latched 1 once the effect is dead           */
    uint32_t bypass_blocks;   /* blocks emitted via bypass (dry passthrough) */
    uint32_t normal_blocks;   /* blocks emitted from the live effect         */
} sb_state_t;

void sb_init(sb_state_t *s);

/* Produce one downstream (DAC-bound) block for an effect node.
 *
 * `alive` is non-zero if the effect ran and produced this block; zero if it has
 * faulted or been killed.  When alive, the effect's output `node_out` is copied
 * to `dst`.  When dead, the effect's input `node_in` is copied to `dst`
 * instead - a clean bypass - so the path never goes silent; if `node_in` is
 * NULL (the dead node had no upstream, e.g. a source), `dst` is silenced.
 * Bypass latches: once dead, the node stays bypassed until it is reloaded and
 * the state is re-initialised.
 *
 * Buffers are `n_words` 32-bit words (float samples copied as raw bits).
 * Returns 1 if the block was bypassed, 0 if it came from the live effect. */
int sb_resolve(sb_state_t *s, int alive,
               const uint32_t *node_out, const uint32_t *node_in,
               uint32_t *dst, uint32_t n_words);

#endif /* ARM64_SAFE_BYPASS_H */
