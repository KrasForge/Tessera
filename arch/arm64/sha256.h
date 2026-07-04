/* arch/arm64/sha256.h - SHA-256 and HMAC-SHA256 (Theme F, issue #125)
 *
 * A self-contained SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), used to
 * authenticate signed plugin packages (package.c).  Integer only, no allocation,
 * no libc - it links into the -mgeneral-regs-only kernel so a package can be
 * verified on-device before a plugin is ever mapped.  Verified against the NIST
 * and RFC 4231 test vectors (make test-arm-sha256).
 */

#ifndef ARM64_SHA256_H
#define ARM64_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t state[8];
    uint64_t total;             /* total bytes hashed */
    uint8_t  buf[SHA256_BLOCK_LEN];
    size_t   buf_len;
} sha256_ctx_t;

void sha256_init  (sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final (sha256_ctx_t *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* HMAC-SHA256 of `data` under `key`, into `out`. */
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* Constant-time compare of two 32-byte digests: returns 1 if equal, 0 if not.
 * The timing does not depend on where the first difference is, so it is safe for
 * comparing a computed MAC against an attacker-supplied one. */
int sha256_equal(const uint8_t a[SHA256_DIGEST_LEN], const uint8_t b[SHA256_DIGEST_LEN]);

#endif /* ARM64_SHA256_H */
