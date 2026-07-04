/* arch/arm64/secureboot.c - secure + measured boot (Theme M21, issue #193).
 * See secureboot.h.
 *
 * Serialized header layout (little-endian, 48 bytes):
 *   0   u32 magic ('TBIM')
 *   4   u16 format_ver
 *   6   u16 reserved
 *   8   u32 load_addr
 *   12  u32 payload_len
 *   16  u8  payload_hash[32]   (SHA-256 of the payload)
 * Then payload[payload_len], then u8 mac[32] over bytes [0 .. 48+payload_len).
 */

#include "secureboot.h"

static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t get_u32(const uint8_t *p)
{ return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

int secureboot_build(uint8_t *buf, int cap, uint32_t load_addr,
                     const uint8_t *payload, uint32_t payload_len,
                     const void *key, size_t key_len)
{
    if (payload_len > (uint32_t)0x7fffffff)
        return -1;
    uint64_t total = (uint64_t)SECUREBOOT_HDR_LEN + payload_len + SECUREBOOT_MAC_LEN;
    if (cap < 0 || total > (uint64_t)cap)
        return -1;

    put_u32(buf + 0,  SECUREBOOT_MAGIC);
    put_u16(buf + 4,  SECUREBOOT_FORMAT_VER);
    put_u16(buf + 6,  0);
    put_u32(buf + 8,  load_addr);
    put_u32(buf + 12, payload_len);

    for (uint32_t i = 0; i < payload_len; i++)
        buf[SECUREBOOT_HDR_LEN + i] = payload[i];

    /* Declared payload hash, then the MAC over header+payload. */
    sha256(buf + SECUREBOOT_HDR_LEN, payload_len, buf + 16);
    hmac_sha256(key, key_len, buf, SECUREBOOT_HDR_LEN + payload_len,
                buf + SECUREBOOT_HDR_LEN + payload_len);
    return (int)total;
}

secureboot_status_t secureboot_verify(const uint8_t *buf, size_t len,
                                      const void *key, size_t key_len,
                                      secureboot_info_t *out)
{
    if (len < (size_t)SECUREBOOT_HDR_LEN + SECUREBOOT_MAC_LEN)
        return SECUREBOOT_ERR_TRUNCATED;
    if (get_u32(buf + 0) != SECUREBOOT_MAGIC)
        return SECUREBOOT_ERR_BAD_MAGIC;
    if (get_u16(buf + 4) != SECUREBOOT_FORMAT_VER)
        return SECUREBOOT_ERR_BAD_FORMAT;

    uint32_t payload_len = get_u32(buf + 12);
    if (payload_len > (uint32_t)0x7fffffff)
        return SECUREBOOT_ERR_TRUNCATED;
    uint64_t need = (uint64_t)SECUREBOOT_HDR_LEN + payload_len + SECUREBOOT_MAC_LEN;
    if (need > (uint64_t)len)
        return SECUREBOOT_ERR_TRUNCATED;

    const uint8_t *payload = buf + SECUREBOOT_HDR_LEN;

    /* Integrity: the payload must match the hash the header declares. */
    uint8_t actual[SHA256_DIGEST_LEN];
    sha256(payload, payload_len, actual);
    if (!sha256_equal(actual, buf + 16))
        return SECUREBOOT_ERR_HASH;

    /* Authenticity: the MAC over header+payload must verify under the key. */
    uint8_t mac[SECUREBOOT_MAC_LEN];
    hmac_sha256(key, key_len, buf, (size_t)SECUREBOOT_HDR_LEN + payload_len, mac);
    if (!sha256_equal(mac, payload + payload_len))
        return SECUREBOOT_ERR_MAC;

    if (out) {
        out->format_ver  = get_u16(buf + 4);
        out->load_addr   = get_u32(buf + 8);
        out->payload     = payload;
        out->payload_len = payload_len;
        for (int i = 0; i < SHA256_DIGEST_LEN; i++)
            out->payload_hash[i] = buf[16 + i];
    }
    return SECUREBOOT_OK;
}

/* ---- measured boot ------------------------------------------------------- */

void secureboot_measure_init(uint8_t m[SHA256_DIGEST_LEN])
{
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) m[i] = 0;
}

void secureboot_measure_extend(uint8_t m[SHA256_DIGEST_LEN], const void *stage, size_t len)
{
    /* PCR-style extend: measure the stage, then fold it into the register. */
    uint8_t stage_hash[SHA256_DIGEST_LEN];
    sha256(stage, len, stage_hash);

    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, m, SHA256_DIGEST_LEN);
    sha256_update(&c, stage_hash, SHA256_DIGEST_LEN);
    sha256_final(&c, m);
}
