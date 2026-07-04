/* arch/arm64/secureboot.h - secure + measured boot (Theme M21, issue #193)
 *
 * Signed plugin packages (issue #125) authenticate a plugin before it is mapped;
 * secure boot extends the same trust down to the boot chain so the *kernel image
 * itself* is authenticated before it runs.  A boot image is a fixed header
 * (magic, versions, load address, payload length, and the payload's SHA-256)
 * followed by the kernel payload and a trailing HMAC-SHA256 over header+payload
 * under a provisioning boot key.
 *
 *   Verified boot  - `secureboot_verify` recomputes the payload hash, checks it
 *   against the header, and authenticates header+payload with the key
 *   (constant-time), rejecting a swapped or corrupted image before the jump.
 *
 *   Measured boot  - a PCR-style measurement register folds each boot stage into
 *   a running hash chain (`m = SHA-256(m || SHA-256(stage))`), so the final value
 *   attests exactly what booted, order and all - even a stage that is loaded
 *   without verification is still measured.
 *
 * Reuses arch/arm64/sha256.c; integer-only, no libc, so it links into the
 * first-stage / kernel.  The header is untrusted input and every length is
 * bounds-checked.  Host-tested (make test-arm-secureboot).
 */

#ifndef ARM64_SECUREBOOT_H
#define ARM64_SECUREBOOT_H

#include "sha256.h"
#include <stdint.h>
#include <stddef.h>

#define SECUREBOOT_MAGIC      0x4d494254u   /* 'TBIM' little-endian */
#define SECUREBOOT_FORMAT_VER 1u
#define SECUREBOOT_HDR_LEN    48            /* serialized header size, see .c   */
#define SECUREBOOT_MAC_LEN    SHA256_DIGEST_LEN

typedef enum {
    SECUREBOOT_OK = 0,
    SECUREBOOT_ERR_TRUNCATED,   /* buffer too small for the declared structure */
    SECUREBOOT_ERR_BAD_MAGIC,
    SECUREBOOT_ERR_BAD_FORMAT,
    SECUREBOOT_ERR_HASH,        /* payload does not match the header's SHA-256  */
    SECUREBOOT_ERR_MAC,         /* authentication failed (tampered / wrong key) */
} secureboot_status_t;

typedef struct {
    uint16_t       format_ver;
    uint32_t       load_addr;
    const uint8_t *payload;
    uint32_t       payload_len;
    uint8_t        payload_hash[SHA256_DIGEST_LEN];
} secureboot_info_t;

/* Serialize a boot image into `buf` (header + payload + MAC), computing the
 * payload's SHA-256 into the header and the MAC over header+payload under `key`.
 * Returns the total byte length, or -1 on overflow.  (Provisioning/test helper.) */
int secureboot_build(uint8_t *buf, int cap, uint32_t load_addr,
                     const uint8_t *payload, uint32_t payload_len,
                     const void *key, size_t key_len);

/* Verify a boot image: bounds, magic, format, the declared payload hash, and the
 * MAC (under `key`).  On SECUREBOOT_OK, fills `out` (its payload pointer aliases
 * into `buf`). */
secureboot_status_t secureboot_verify(const uint8_t *buf, size_t len,
                                      const void *key, size_t key_len,
                                      secureboot_info_t *out);

/* ---- measured boot ------------------------------------------------------- */

/* Reset a measurement register to zero. */
void secureboot_measure_init(uint8_t m[SHA256_DIGEST_LEN]);

/* Fold one boot stage into the measurement: m = SHA-256(m || SHA-256(stage)).
 * Order-sensitive, so the final value attests the exact boot sequence. */
void secureboot_measure_extend(uint8_t m[SHA256_DIGEST_LEN], const void *stage, size_t len);

#endif /* ARM64_SECUREBOOT_H */
