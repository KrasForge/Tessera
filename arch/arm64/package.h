/* arch/arm64/package.h - signed plugin package format (Theme F, issue #125)
 *
 * The completion of the untrusted-plugin thesis: a plugin arrives as a package
 * that is authenticated before it is ever mapped.  A package is a fixed header
 * (magic, format/ABI versions, a signing-key id, capability flags, and the
 * plugin name) followed by the ELF payload and a trailing 32-byte MAC.
 *
 * Authenticity + integrity come from an HMAC-SHA256 over the header and payload
 * under a provisioning key (sha256.c); this is symmetric, so a production build
 * would swap in an asymmetric signature, but the format, the bounds-checked
 * parse, the revocation check, and the constant-time compare are the parts that
 * matter and they are unchanged by the primitive.  Key ids can be revoked, so a
 * compromised signer is rejected without a firmware change.
 *
 * The verifier is fed an untrusted blob, so every length is checked against the
 * buffer before use.  Integer only, no allocation, no libc; host-tested
 * (make test-arm-package).
 */

#ifndef ARM64_PACKAGE_H
#define ARM64_PACKAGE_H

#include "sha256.h"
#include <stdint.h>
#include <stddef.h>

#define PKG_MAGIC        0x474b5054u   /* 'TPKG' little-endian */
#define PKG_FORMAT_VER   1u
#define PKG_NAME_LEN     32
#define PKG_HEADER_LEN   52            /* serialized header size, see package.c */
#define PKG_MAC_LEN      SHA256_DIGEST_LEN

/* Capability flags a package declares (checked by the loader/sandbox later). */
#define PKG_FLAG_WANTS_INPUT   0x1u    /* uses the capture input          */
#define PKG_FLAG_WANTS_MIDI    0x2u    /* consumes note/CC events         */

typedef enum {
    PKG_OK = 0,
    PKG_ERR_TRUNCATED,     /* buffer too small for the declared structure */
    PKG_ERR_BAD_MAGIC,
    PKG_ERR_BAD_FORMAT,    /* unsupported format version                  */
    PKG_ERR_BAD_MAC,       /* authentication failed (tampered/wrong key)  */
    PKG_ERR_REVOKED,       /* the signing key id is on the revocation list*/
} pkg_status_t;

typedef struct {
    uint16_t     format_ver;
    uint16_t     abi_ver;
    uint32_t     key_id;
    uint32_t     flags;
    char         name[PKG_NAME_LEN + 1];   /* NUL-terminated */
    const uint8_t *payload;                /* into the input buffer */
    uint32_t     payload_len;
} pkg_info_t;

/* Serialize a package into `buf` (capacity `cap`): header + payload + MAC, with
 * the MAC computed over header+payload under `key`.  Returns the total byte
 * length, or -1 on overflow.  (Build/test/provisioning helper.) */
int pkg_build(uint8_t *buf, int cap,
              uint16_t abi_ver, uint32_t key_id, uint32_t flags, const char *name,
              const uint8_t *payload, uint32_t payload_len,
              const void *key, size_t key_len);

/* Verify a package blob: bounds, magic, format, MAC (under `key`), and that the
 * key id is not among `revoked[0..n_revoked)`.  On PKG_OK, fills `out` (its
 * payload pointer aliases into `buf`).  `revoked` may be NULL when n_revoked==0. */
pkg_status_t pkg_verify(const uint8_t *buf, size_t len,
                        const void *key, size_t key_len,
                        const uint32_t *revoked, int n_revoked,
                        pkg_info_t *out);

#endif /* ARM64_PACKAGE_H */
