/* tests/arm64/package_test.c - host unit tests for the signed plugin package
 * format (Theme F, issue #125).
 *
 * Build/run via:  make test-arm-package
 */

#include "package.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static const uint8_t KEY[16] = { 'p','r','o','v','i','s','i','o','n',0,1,2,3,4,5,6 };

/* A tiny fake ELF payload. */
static const uint8_t PAYLOAD[] = { 0x7f,'E','L','F', 1,2,3,4,5,6,7,8, 42,42,42 };

static int build(uint8_t *buf, int cap, uint32_t key_id, uint32_t flags)
{
    return pkg_build(buf, cap, /*abi*/ 0x0101u, key_id, flags, "reverb",
                     PAYLOAD, sizeof PAYLOAD, KEY, sizeof KEY);
}

static void test_roundtrip(void)
{
    printf("- a well-formed package verifies and exposes its payload\n");
    uint8_t buf[256];
    int n = build(buf, sizeof buf, 7, PKG_FLAG_WANTS_MIDI);
    CHECK(n == PKG_HEADER_LEN + (int)sizeof PAYLOAD + PKG_MAC_LEN, "build size is header+payload+MAC");

    pkg_info_t info;
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_OK, "verify ok");
    CHECK(info.abi_ver == 0x0101u, "ABI version parsed");
    CHECK(info.key_id == 7, "key id parsed");
    CHECK(info.flags == PKG_FLAG_WANTS_MIDI, "flags parsed");
    CHECK(strcmp(info.name, "reverb") == 0, "name parsed");
    CHECK(info.payload_len == sizeof PAYLOAD &&
          memcmp(info.payload, PAYLOAD, sizeof PAYLOAD) == 0, "payload exposed intact");
}

static void test_tamper(void)
{
    printf("- tampering is caught by the MAC\n");
    uint8_t buf[256];
    int n = build(buf, sizeof buf, 7, 0);
    pkg_info_t info;

    /* Flip a payload byte. */
    buf[PKG_HEADER_LEN + 2] ^= 0x01;
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_ERR_BAD_MAC,
          "a flipped payload byte fails the MAC");
    buf[PKG_HEADER_LEN + 2] ^= 0x01;   /* restore */

    /* Flip a header byte (the flags). */
    buf[12] ^= 0x02;
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_ERR_BAD_MAC,
          "a flipped header byte fails the MAC");
    buf[12] ^= 0x02;

    /* Wrong key. */
    uint8_t badkey[16]; memcpy(badkey, KEY, 16); badkey[0] ^= 0xff;
    CHECK(pkg_verify(buf, n, badkey, sizeof badkey, NULL, 0, &info) == PKG_ERR_BAD_MAC,
          "the wrong key fails the MAC");

    /* Restored + right key verifies again. */
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_OK,
          "restored package verifies again");
}

static void test_revocation(void)
{
    printf("- a revoked signing key id is rejected (after authentication)\n");
    uint8_t buf[256];
    int n = build(buf, sizeof buf, 42, 0);
    pkg_info_t info;
    uint32_t revoked[] = { 5, 42, 99 };
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, revoked, 3, &info) == PKG_ERR_REVOKED,
          "key id 42 on the revocation list is rejected");
    uint32_t other[] = { 5, 99 };
    CHECK(pkg_verify(buf, n, KEY, sizeof KEY, other, 2, &info) == PKG_OK,
          "a key id not on the list still verifies");
}

static void test_malformed(void)
{
    printf("- malformed blobs are rejected before the payload is trusted\n");
    uint8_t buf[256];
    int n = build(buf, sizeof buf, 1, 0);
    pkg_info_t info;

    CHECK(pkg_verify(buf, 10, KEY, sizeof KEY, NULL, 0, &info) == PKG_ERR_TRUNCATED,
          "too short for a header + MAC");

    uint8_t bad[256]; memcpy(bad, buf, n);
    bad[0] ^= 0xff;   /* corrupt the magic */
    CHECK(pkg_verify(bad, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_ERR_BAD_MAGIC,
          "bad magic rejected");

    /* An oversized declared payload_len must not read past the buffer. */
    memcpy(bad, buf, n);
    bad[16] = 0xff; bad[17] = 0xff; bad[18] = 0xff; bad[19] = 0x7f;  /* huge len */
    CHECK(pkg_verify(bad, n, KEY, sizeof KEY, NULL, 0, &info) == PKG_ERR_TRUNCATED,
          "an over-long payload length is rejected, not over-read");
}

int main(void)
{
    printf("=== Tessera signed-package tests (Theme F, #125) ===\n");
    test_roundtrip();
    test_tamper();
    test_revocation();
    test_malformed();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
