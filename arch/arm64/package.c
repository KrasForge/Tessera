/* arch/arm64/package.c - signed plugin package format (Theme F, issue #125).
 * See package.h.
 *
 * Serialized header layout (little-endian, 52 bytes):
 *   0   u32 magic         ('TPKG')
 *   4   u16 format_ver
 *   6   u16 abi_ver
 *   8   u32 key_id
 *   12  u32 flags
 *   16  u32 payload_len
 *   20  char name[32]     (NUL-padded)
 * Then payload[payload_len], then u8 mac[32] over bytes [0 .. 52+payload_len).
 */

#include "package.h"

static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

int pkg_build(uint8_t *buf, int cap,
              uint16_t abi_ver, uint32_t key_id, uint32_t flags, const char *name,
              const uint8_t *payload, uint32_t payload_len,
              const void *key, size_t key_len)
{
    /* Guard the arithmetic so a huge payload_len cannot wrap the size check. */
    if (payload_len > (uint32_t)0x7fffffff)
        return -1;
    uint64_t total = (uint64_t)PKG_HEADER_LEN + payload_len + PKG_MAC_LEN;
    if (cap < 0 || total > (uint64_t)cap)
        return -1;

    put_u32(buf + 0,  PKG_MAGIC);
    put_u16(buf + 4,  PKG_FORMAT_VER);
    put_u16(buf + 6,  abi_ver);
    put_u32(buf + 8,  key_id);
    put_u32(buf + 12, flags);
    put_u32(buf + 16, payload_len);
    /* Copy the name, NUL-padded; stop reading `name` at its terminator so a
     * short name is never over-read. */
    int done = 0;
    for (int i = 0; i < PKG_NAME_LEN; i++) {
        if (!done && name && name[i]) buf[20 + i] = (uint8_t)name[i];
        else { buf[20 + i] = 0; done = 1; }
    }

    for (uint32_t i = 0; i < payload_len; i++)
        buf[PKG_HEADER_LEN + i] = payload[i];

    hmac_sha256(key, key_len, buf, PKG_HEADER_LEN + payload_len,
                buf + PKG_HEADER_LEN + payload_len);
    return (int)total;
}

pkg_status_t pkg_verify(const uint8_t *buf, size_t len,
                        const void *key, size_t key_len,
                        const uint32_t *revoked, int n_revoked,
                        pkg_info_t *out)
{
    /* Must at least hold a header and a MAC. */
    if (len < (size_t)PKG_HEADER_LEN + PKG_MAC_LEN)
        return PKG_ERR_TRUNCATED;
    if (get_u32(buf + 0) != PKG_MAGIC)
        return PKG_ERR_BAD_MAGIC;
    if (get_u16(buf + 4) != PKG_FORMAT_VER)
        return PKG_ERR_BAD_FORMAT;

    uint32_t payload_len = get_u32(buf + 16);
    /* The declared payload plus the fixed header and MAC must fit exactly within
     * the buffer (checked without overflow: payload_len is bounded first). */
    if (payload_len > (uint32_t)0x7fffffff)
        return PKG_ERR_TRUNCATED;
    uint64_t need = (uint64_t)PKG_HEADER_LEN + payload_len + PKG_MAC_LEN;
    if (need > (uint64_t)len)
        return PKG_ERR_TRUNCATED;

    /* Recompute and compare the MAC in constant time before trusting anything
     * else in the blob. */
    uint8_t mac[PKG_MAC_LEN];
    hmac_sha256(key, key_len, buf, (size_t)PKG_HEADER_LEN + payload_len, mac);
    const uint8_t *have = buf + PKG_HEADER_LEN + payload_len;
    if (!sha256_equal(mac, have))
        return PKG_ERR_BAD_MAC;

    /* Authenticated: now the key id is trustworthy, so check revocation. */
    uint32_t key_id = get_u32(buf + 8);
    for (int i = 0; i < n_revoked; i++)
        if (revoked[i] == key_id)
            return PKG_ERR_REVOKED;

    if (out) {
        out->format_ver  = get_u16(buf + 4);
        out->abi_ver     = get_u16(buf + 6);
        out->key_id      = key_id;
        out->flags       = get_u32(buf + 12);
        for (int i = 0; i < PKG_NAME_LEN; i++) out->name[i] = (char)buf[20 + i];
        out->name[PKG_NAME_LEN] = '\0';
        out->payload     = buf + PKG_HEADER_LEN;
        out->payload_len = payload_len;
    }
    return PKG_OK;
}
