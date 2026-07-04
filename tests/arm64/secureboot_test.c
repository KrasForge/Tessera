/* tests/arm64/secureboot_test.c - host unit tests for secure + measured boot
 * (Theme M21, issue #193).
 *
 * Build/run via:  make test-arm-secureboot
 */

#include "secureboot.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static const uint8_t KEY[16] = { 'b','o','o','t','k','e','y',0,9,8,7,6,5,4,3,2 };

/* A stand-in "kernel image". */
static uint8_t PAYLOAD[300];
static void fill_payload(void) { for (int i = 0; i < 300; i++) PAYLOAD[i] = (uint8_t)(i * 7 + 3); }

static void test_verify_roundtrip(void)
{
    printf("- a well-formed, signed image verifies\n");
    static uint8_t buf[512];
    int n = secureboot_build(buf, sizeof buf, 0x80000, PAYLOAD, sizeof PAYLOAD, KEY, sizeof KEY);
    CHECK(n == SECUREBOOT_HDR_LEN + (int)sizeof PAYLOAD + SECUREBOOT_MAC_LEN,
          "build size is header + payload + MAC");

    secureboot_info_t info;
    CHECK(secureboot_verify(buf, n, KEY, sizeof KEY, &info) == SECUREBOOT_OK, "verify ok");
    CHECK(info.load_addr == 0x80000, "load address parsed");
    CHECK(info.payload_len == sizeof PAYLOAD &&
          memcmp(info.payload, PAYLOAD, sizeof PAYLOAD) == 0, "payload exposed intact");
}

static void test_tamper(void)
{
    printf("- tampering is caught (hash then MAC)\n");
    static uint8_t buf[512];
    int n = secureboot_build(buf, sizeof buf, 0x80000, PAYLOAD, sizeof PAYLOAD, KEY, sizeof KEY);
    secureboot_info_t info;

    /* Flip a payload byte: the declared hash no longer matches. */
    buf[SECUREBOOT_HDR_LEN + 40] ^= 0x01;
    CHECK(secureboot_verify(buf, n, KEY, sizeof KEY, &info) == SECUREBOOT_ERR_HASH,
          "a flipped payload byte fails the integrity hash");
    buf[SECUREBOOT_HDR_LEN + 40] ^= 0x01;

    /* Flip the load address: payload hash still matches, but the MAC (over the
     * header too) fails. */
    buf[8] ^= 0x10;
    CHECK(secureboot_verify(buf, n, KEY, sizeof KEY, &info) == SECUREBOOT_ERR_MAC,
          "a flipped header byte fails the MAC");
    buf[8] ^= 0x10;

    /* Wrong key. */
    uint8_t badkey[16]; memcpy(badkey, KEY, 16); badkey[0] ^= 0xff;
    CHECK(secureboot_verify(buf, n, badkey, sizeof badkey, &info) == SECUREBOOT_ERR_MAC,
          "the wrong key fails the MAC");

    CHECK(secureboot_verify(buf, n, KEY, sizeof KEY, &info) == SECUREBOOT_OK,
          "the untouched image verifies again");
}

static void test_malformed(void)
{
    printf("- malformed images are rejected before the payload is trusted\n");
    static uint8_t buf[512];
    int n = secureboot_build(buf, sizeof buf, 0x80000, PAYLOAD, sizeof PAYLOAD, KEY, sizeof KEY);
    secureboot_info_t info;

    CHECK(secureboot_verify(buf, 20, KEY, sizeof KEY, &info) == SECUREBOOT_ERR_TRUNCATED,
          "too short for a header + MAC");
    uint8_t bad[512]; memcpy(bad, buf, n);
    bad[0] ^= 0xff;
    CHECK(secureboot_verify(bad, n, KEY, sizeof KEY, &info) == SECUREBOOT_ERR_BAD_MAGIC,
          "bad magic rejected");
    /* An over-long declared payload length must not read past the buffer. */
    memcpy(bad, buf, n);
    bad[12] = 0xff; bad[13] = 0xff; bad[14] = 0xff; bad[15] = 0x7f;
    CHECK(secureboot_verify(bad, n, KEY, sizeof KEY, &info) == SECUREBOOT_ERR_TRUNCATED,
          "an over-long payload length is rejected, not over-read");
}

static void test_measured_boot(void)
{
    printf("- measured boot is deterministic and order-sensitive\n");
    const char *s1 = "first-stage", *s2 = "kernel", *s3 = "initrd";

    uint8_t a[32]; secureboot_measure_init(a);
    secureboot_measure_extend(a, s1, 11);
    secureboot_measure_extend(a, s2, 6);
    secureboot_measure_extend(a, s3, 6);

    /* The same stages in the same order reproduce the measurement. */
    uint8_t b[32]; secureboot_measure_init(b);
    secureboot_measure_extend(b, s1, 11);
    secureboot_measure_extend(b, s2, 6);
    secureboot_measure_extend(b, s3, 6);
    CHECK(memcmp(a, b, 32) == 0, "same stages, same order -> same measurement");

    /* A different order gives a different measurement. */
    uint8_t c[32]; secureboot_measure_init(c);
    secureboot_measure_extend(c, s2, 6);
    secureboot_measure_extend(c, s1, 11);
    secureboot_measure_extend(c, s3, 6);
    CHECK(memcmp(a, c, 32) != 0, "reordering the stages changes the measurement");

    /* A changed stage changes the measurement. */
    uint8_t d[32]; secureboot_measure_init(d);
    secureboot_measure_extend(d, s1, 11);
    secureboot_measure_extend(d, "kerneL", 6);      /* one byte different */
    secureboot_measure_extend(d, s3, 6);
    CHECK(memcmp(a, d, 32) != 0, "changing a stage changes the measurement");

    /* An empty chain is the zero register (a known starting point). */
    uint8_t z[32]; secureboot_measure_init(z);
    uint8_t zero[32]; memset(zero, 0, 32);
    CHECK(memcmp(z, zero, 32) == 0, "the initial measurement is zero");
}

int main(void)
{
    fill_payload();
    printf("=== Tessera secure/measured boot tests (M21, #193) ===\n");
    test_verify_roundtrip();
    test_tamper();
    test_malformed();
    test_measured_boot();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
