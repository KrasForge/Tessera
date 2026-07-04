/* tests/arm64/blackbox_test.c - host unit tests for the crash black-box
 * (Theme A: reliability).
 *
 * The recorder keeps the last N DAC-bound blocks; on a fault it freezes the
 * faulting plugin's identity and serialises a snapshot that must survive a
 * reboot and reject corruption. The logic is pure, so it is checked here for
 * exact behaviour: the circular retention, the capture latch, a bit-exact
 * serialise/parse round-trip (the "survives reboot" property), and rejection of
 * truncation, bad magic, length mismatch, and any bit flip (checksum).
 *
 * Build/run via:  make test-arm-blackbox
 */

#include "blackbox.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define BW 8u

static void mkblock(uint32_t *b, uint32_t tag)
{
    for (uint32_t w = 0; w < BW; w++) b[w] = tag * 1000u + w;
}
static int block_is(const uint32_t *b, uint32_t tag)
{
    for (uint32_t w = 0; w < BW; w++) if (b[w] != tag * 1000u + w) return 0;
    return 1;
}

static void test_retention_under_n(void)
{
    printf("- fewer than N blocks: all retained, in order\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW], out[BW];
    for (uint32_t k = 0; k < 2; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    CHECK(bb.count == 2u && bb.total == 2u, "count=2 total=2");
    CHECK(bb_chrono(&bb, 0, out) && block_is(out, 0), "oldest is block 0");
    CHECK(bb_chrono(&bb, 1, out) && block_is(out, 1), "newest is block 1");
    CHECK(bb_chrono(&bb, 2, out) == 0, "no third block");
}

static void test_retention_over_n(void)
{
    printf("- more than N blocks: only the last N kept, oldest-first\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW], out[BW];
    for (uint32_t k = 0; k < 6; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    CHECK(bb.count == BB_BLOCKS && bb.total == 6u, "count=N total=6");
    /* last N of 0..5 are 2,3,4,5 */
    int ok = 1;
    for (uint32_t i = 0; i < BB_BLOCKS; i++)
        if (!bb_chrono(&bb, i, out) || !block_is(out, 2u + i)) ok = 0;
    CHECK(ok, "retained blocks are 2,3,4,5 in chronological order");
}

static void test_capture_latches(void)
{
    printf("- capture freezes the faulting plugin and latches\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW];
    for (uint32_t k = 0; k < 3; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    bb_capture(&bb, 7u, "evilsynth", BB_CAUSE_MMU);
    CHECK(bb.captured == 1u, "captured");
    CHECK(bb.fault_pid == 7u, "faulting pid recorded");
    CHECK(bb.fault_cause == (uint32_t)BB_CAUSE_MMU, "cause recorded");
    CHECK(bb.fault_block == 3u, "fault block index == blocks seen (3)");
    CHECK(bb.fault_name[0] == 'e' && bb.fault_name[8] == 'h', "name recorded");
    bb_capture(&bb, 9u, "other", BB_CAUSE_SVC);      /* must be ignored */
    CHECK(bb.fault_pid == 7u && bb.fault_cause == (uint32_t)BB_CAUSE_MMU,
          "later capture ignored - the first crash wins");
}

static void test_name_truncation(void)
{
    printf("- an over-long plugin name is truncated and NUL-terminated\n");
    blackbox_t bb; bb_init(&bb, BW);
    bb_capture(&bb, 1u, "a-very-long-plugin-name-indeed", BB_CAUSE_BUDGET);
    CHECK(bb.fault_name[BB_NAME_MAX - 1u] == '\0', "last byte is NUL");
    int within = 1;
    for (uint32_t i = 0; i < BB_NAME_MAX - 1u; i++)
        if (bb.fault_name[i] != "a-very-long-plugin-name-indeed"[i]) within = 0;
    CHECK(within, "the first BB_NAME_MAX-1 chars are preserved");
}

static void test_roundtrip_survives_reboot(void)
{
    printf("- serialise -> reboot -> parse recovers identity and blocks exactly\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW], out[BW];
    for (uint32_t k = 0; k < 5; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    bb_capture(&bb, 42u, "hog", BB_CAUSE_BUDGET);

    uint8_t buf[1024];
    size_t n = bb_serialize(&bb, buf, sizeof buf);
    CHECK(n == bb_serialized_size(&bb) && n > 0, "serialised to the exact size");

    /* the reboot: a fresh, zeroed recorder recovers from the store alone */
    blackbox_t rb; bb_init(&rb, 1u);
    CHECK(bb_parse(buf, n, &rb) == 1, "parse succeeds");
    CHECK(rb.fault_pid == 42u && rb.fault_cause == (uint32_t)BB_CAUSE_BUDGET,
          "faulting plugin identity recovered");
    CHECK(rb.fault_block == 5u, "fault block index recovered");
    CHECK(rb.fault_name[0] == 'h' && rb.fault_name[1] == 'o' && rb.fault_name[2] == 'g',
          "faulting plugin name recovered");
    CHECK(rb.count == BB_BLOCKS, "the last N blocks were recovered");
    int ok = 1;
    for (uint32_t i = 0; i < BB_BLOCKS; i++)          /* last 4 of 0..4 => 1,2,3,4 */
        if (!bb_chrono(&rb, i, out) || !block_is(out, 1u + i)) ok = 0;
    CHECK(ok, "recovered blocks are bit-exact and in order");
}

static void test_corruption_rejected(void)
{
    printf("- a corrupt store is rejected, never yields a bogus post-mortem\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW];
    for (uint32_t k = 0; k < 4; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    bb_capture(&bb, 3u, "crash", BB_CAUSE_MMU);

    uint8_t buf[1024], tmp[1024];
    size_t n = bb_serialize(&bb, buf, sizeof buf);
    blackbox_t rb;

    for (size_t i = 0; i < n; i++) tmp[i] = buf[i];
    CHECK(bb_parse(tmp, n, &rb) == 1, "the clean store parses");

    for (size_t i = 0; i < n; i++) tmp[i] = buf[i];
    tmp[n / 2] ^= 0x20;                                /* flip a data bit */
    CHECK(bb_parse(tmp, n, &rb) == 0, "a flipped block byte is rejected (checksum)");

    for (size_t i = 0; i < n; i++) tmp[i] = buf[i];
    tmp[0] ^= 0xFF;                                    /* wreck the magic */
    CHECK(bb_parse(tmp, n, &rb) == 0, "a bad magic is rejected");

    for (size_t i = 0; i < n; i++) tmp[i] = buf[i];
    tmp[n - 1] ^= 0x01;                                /* flip a checksum bit */
    CHECK(bb_parse(tmp, n, &rb) == 0, "a flipped checksum byte is rejected");

    CHECK(bb_parse(buf, n - 1u, &rb) == 0, "a truncated store is rejected");
    for (size_t i = 0; i < n; i++) tmp[i] = buf[i];
    tmp[n] = 0;
    CHECK(bb_parse(tmp, n + 1u, &rb) == 0, "a length mismatch (trailing byte) is rejected");
}

static void test_serialized_size(void)
{
    printf("- serialised size is header + retained blocks + checksum\n");
    blackbox_t bb; bb_init(&bb, BW);
    uint32_t blk[BW];
    for (uint32_t k = 0; k < 3; k++) { mkblock(blk, k); bb_record(&bb, blk); }
    size_t hdr = 9u * 4u + BB_NAME_MAX;
    CHECK(bb_serialized_size(&bb) == hdr + 3u * BW * 4u + 4u, "size matches the layout");
}

int main(void)
{
    printf("=== Tessera crash black-box tests (Theme A) ===\n");
    test_retention_under_n();
    test_retention_over_n();
    test_capture_latches();
    test_name_truncation();
    test_roundtrip_survives_reboot();
    test_corruption_rejected();
    test_serialized_size();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
