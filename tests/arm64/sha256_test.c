/* tests/arm64/sha256_test.c - host unit tests for SHA-256 / HMAC-SHA256
 * (Theme F, issue #125), checked against the NIST and RFC 4231 vectors.
 *
 * Build/run via:  make test-arm-sha256
 */

#include "sha256.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

/* Compare a 32-byte digest against a 64-char lowercase hex string. */
static int hexeq(const uint8_t d[32], const char *hex)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        char hi = H[d[i] >> 4], lo = H[d[i] & 0xf];
        if (hex[i*2] != hi || hex[i*2+1] != lo) return 0;
    }
    return hex[64] == '\0';
}

static void test_sha256_vectors(void)
{
    printf("- SHA-256 NIST vectors\n");
    uint8_t d[32];
    sha256("", 0, d);
    CHECK(hexeq(d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "empty string");
    sha256("abc", 3, d);
    CHECK(hexeq(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "\"abc\"");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m, strlen(m), d);
    CHECK(hexeq(d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
          "56-byte message (two-block padding boundary)");
}

static void test_sha256_streaming(void)
{
    printf("- streaming update matches one-shot, across block boundaries\n");
    uint8_t big[1000];
    for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i * 31 + 7);
    uint8_t oneshot[32], streamed[32];
    sha256(big, sizeof big, oneshot);

    sha256_ctx_t c; sha256_init(&c);
    /* Feed in awkward chunk sizes to exercise the buffering. */
    size_t off = 0;
    size_t chunks[] = { 1, 63, 64, 65, 100, 200, 300, 207 };  /* sums to 1000 */
    for (unsigned i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        sha256_update(&c, big + off, chunks[i]);
        off += chunks[i];
    }
    sha256_final(&c, streamed);
    CHECK(off == 1000, "chunk sizes cover the whole message");
    CHECK(memcmp(oneshot, streamed, 32) == 0, "streamed == one-shot digest");
}

static void test_hmac_vectors(void)
{
    printf("- HMAC-SHA256 RFC 4231 vectors\n");
    uint8_t d[32];
    uint8_t key1[20];
    for (int i = 0; i < 20; i++) key1[i] = 0x0b;
    hmac_sha256(key1, 20, "Hi There", 8, d);
    CHECK(hexeq(d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
          "case 1 (20-byte key)");

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    CHECK(hexeq(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
          "case 2 (short ASCII key)");

    /* A key longer than the 64-byte block is hashed down first (RFC 4231 case 6,
     * 131-byte key). */
    uint8_t longkey[131];
    for (int i = 0; i < 131; i++) longkey[i] = 0xaa;
    hmac_sha256(longkey, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
    CHECK(hexeq(d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
          "case 6 (131-byte oversize key hashed down)");
}

static void test_equal(void)
{
    printf("- constant-time digest compare\n");
    uint8_t a[32], b[32];
    for (int i = 0; i < 32; i++) a[i] = b[i] = (uint8_t)i;
    CHECK(sha256_equal(a, b) == 1, "identical digests compare equal");
    b[31] ^= 1;
    CHECK(sha256_equal(a, b) == 0, "a single differing bit compares unequal");
    b[31] ^= 1; b[0] ^= 0x80;
    CHECK(sha256_equal(a, b) == 0, "a difference in the first byte too");
}

int main(void)
{
    printf("=== Tessera SHA-256 / HMAC-SHA256 tests (Theme F, #125) ===\n");
    test_sha256_vectors();
    test_sha256_streaming();
    test_hmac_vectors();
    test_equal();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
