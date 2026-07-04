/* arch/arm64/blackbox.h - crash black-box: post-mortem across a reboot
 *                         (Theme A: reliability)
 *
 * When a plugin faults, isolation lets Tessera catch and kill it while the audio
 * keeps running (M8/M12) - but the evidence of *why* it died is gone by the next
 * reboot.  The black box is a flight recorder: it keeps the last N DAC-bound
 * blocks in a small circular buffer, and when a plugin is killed it freezes a
 * snapshot - those last blocks plus the faulting plugin's identity and the
 * fault cause - and serialises it to a reserved store that survives a reboot.
 * After the reboot the snapshot can be parsed back for post-mortem: which
 * plugin, why, and exactly what the audio was doing in the blocks leading up to
 * the fault.
 *
 * This is trivial *because* of isolation - the fault is contained to one address
 * space, so the recorder and the store are intact after the kill.  On a
 * single-process host a crash takes down the whole engine, recorder and all,
 * and there is nothing left to persist.
 *
 * The recorder and the serialisation format are pure and FP-free (blocks are
 * copied as raw 32-bit words), so they are unit-tested on the host
 * (make test-arm-blackbox) and demonstrated end to end - real fault, capture,
 * reboot, recover - on QEMU virt (make test-arm-blackbox-qemu).
 */

#ifndef ARM64_BLACKBOX_H
#define ARM64_BLACKBOX_H

#include <stdint.h>
#include <stddef.h>

#define BB_BLOCKS           4u          /* last N blocks retained             */
#define BB_MAX_BLOCK_WORDS  512u        /* max words/block (256 stereo frames)*/
#define BB_NAME_MAX         16u         /* faulting-plugin name capacity      */
#define BB_MAGIC            0x424B4258u /* 'BKBX'                             */
#define BB_VERSION          1u

/* Why the plugin was killed - the same three the sandbox already enforces. */
typedef enum {
    BB_CAUSE_NONE   = 0,
    BB_CAUSE_MMU    = 1,   /* EL0 memory abort (wild/null access)  */
    BB_CAUSE_SVC    = 2,   /* forbidden syscall from the audio path */
    BB_CAUSE_BUDGET = 3,   /* CPU-budget overrun (M12)             */
} bb_cause_t;

typedef struct {
    uint32_t block_words;                /* words per recorded block          */
    uint32_t head;                       /* next slot to write                */
    uint32_t count;                      /* valid blocks (<= BB_BLOCKS)        */
    uint32_t total;                      /* blocks recorded, free-running      */
    uint32_t ring[BB_BLOCKS][BB_MAX_BLOCK_WORDS];

    uint32_t captured;                   /* 1 once a fault snapshot is frozen  */
    uint32_t fault_pid;
    uint32_t fault_cause;                /* bb_cause_t                        */
    uint32_t fault_block;                /* `total` at the moment of the fault */
    char     fault_name[BB_NAME_MAX];    /* NUL-padded                        */
} blackbox_t;

/* Reset the recorder.  `block_words` is clamped to BB_MAX_BLOCK_WORDS. */
void bb_init(blackbox_t *bb, uint32_t block_words);

/* Record one DAC-bound block (`block_words` raw words) into the ring. */
void bb_record(blackbox_t *bb, const uint32_t *block);

/* The i-th retained block in chronological order (0 = oldest kept), copied into
 * `out` (`block_words` words).  Returns 1 on success, 0 if i >= count. */
int bb_chrono(const blackbox_t *bb, uint32_t i, uint32_t *out);

/* Freeze the crash snapshot: the faulting plugin's pid, name, and cause, plus
 * the blocks already recorded.  Latches - the first capture wins (the plugin
 * that actually crashed), later ones are ignored. */
void bb_capture(blackbox_t *bb, uint32_t pid, const char *name, bb_cause_t cause);

/* Exact serialised size for the current snapshot (header + retained blocks +
 * checksum), in bytes. */
size_t bb_serialized_size(const blackbox_t *bb);

/* Serialise the snapshot into `buf` (little-endian, checksummed).  Returns the
 * number of bytes written, or 0 if `cap` is too small. */
size_t bb_serialize(const blackbox_t *bb, uint8_t *buf, size_t cap);

/* Parse a serialised snapshot into `out`.  Returns 1 on success, or 0 if the
 * blob is truncated, has the wrong magic/version, or fails its checksum (a
 * corrupt store must never yield a bogus post-mortem). */
int bb_parse(const uint8_t *buf, size_t len, blackbox_t *out);

#endif /* ARM64_BLACKBOX_H */
