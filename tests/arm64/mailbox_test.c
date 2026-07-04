/* tests/arm64/mailbox_test.c - host unit tests for the VideoCore property
 * mailbox message format (Theme M10, issue #105).
 *
 * The hardware doorbell (mbox_call) needs a real BCM2711 and is not exercised
 * here; the message serialisation and parsing are pure and fully testable.
 *
 * Build/run via:  make test-arm-mailbox
 */

#include "mailbox.h"

#include <stdio.h>
#include <stdint.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

static void test_build(void)
{
    printf("- build a property message with two tags\n");
    uint32_t buf[32];
    mbox_builder_t b;
    mbox_init(&b, buf, 32);

    int rev = mbox_add_tag(&b, MBOX_TAG_GET_BOARD_REV, NULL, 0, 1);  /* 1 resp word */
    uint32_t setclk[2] = { MBOX_CLOCK_PCM, 48000u * 256u };          /* clk id, rate */
    int clk = mbox_add_tag(&b, MBOX_TAG_SET_CLOCK_RATE, setclk, 2, 2);
    int words = mbox_finish(&b);

    CHECK(rev > 0 && clk > 0 && words > 0, "tags added and message finished");
    CHECK(buf[0] == (uint32_t)words * 4u, "word 0 is the total size in bytes");
    CHECK(buf[1] == MBOX_CODE_REQUEST, "word 1 is the request code");
    CHECK(buf[words - 1] == MBOX_TAG_END, "the message ends with the end tag");

    /* The SET_CLOCK_RATE request words are present. */
    CHECK(buf[clk] == MBOX_CLOCK_PCM && buf[clk + 1] == 48000u * 256u,
          "SET_CLOCK_RATE carries the clock id and rate");
    /* GET_BOARD_REV reserved a 1-word value buffer (4 bytes). */
    CHECK(buf[rev - 2] == 4u, "GET_BOARD_REV value buffer is 4 bytes");
}

static void test_find_and_response(void)
{
    printf("- locate tags and read a simulated firmware response\n");
    uint32_t buf[32];
    mbox_builder_t b;
    mbox_init(&b, buf, 32);
    int rev = mbox_add_tag(&b, MBOX_TAG_GET_BOARD_REV, NULL, 0, 1);
    mbox_finish(&b);

    const uint32_t *val; int nw;
    CHECK(mbox_find_tag(buf, 32, MBOX_TAG_GET_BOARD_REV, &val, &nw) == 0,
          "GET_BOARD_REV located");
    CHECK(val == &buf[rev] && nw == 1, "value pointer and width match the builder");
    CHECK(mbox_find_tag(buf, 32, MBOX_TAG_GET_FIRMWARE, &val, &nw) == -1,
          "an absent tag is not found");

    /* Simulate what the firmware writes back: overall OK, the tag marked as a
     * response, and a board-revision value. */
    buf[1] = MBOX_CODE_RESP_OK;
    buf[rev - 1] |= MBOX_TAG_RESP_BIT | 4u;   /* resp bit + 4 response bytes */
    buf[rev] = 0x00c03111u;                    /* a CM4 board revision */

    CHECK(mbox_response_ok(buf), "response reports overall success");
    mbox_find_tag(buf, 32, MBOX_TAG_GET_BOARD_REV, &val, &nw);
    CHECK(mbox_tag_has_response(buf, val), "the tag is marked as answered");
    CHECK(val[0] == 0x00c03111u, "the board revision reads back");
}

static void test_malformed_and_overflow(void)
{
    printf("- malformed responses and builder overflow are handled\n");
    uint32_t buf[32];
    mbox_builder_t b;
    mbox_init(&b, buf, 32);
    mbox_add_tag(&b, MBOX_TAG_GET_BOARD_REV, NULL, 0, 1);
    mbox_finish(&b);

    /* Corrupt a tag's value-size so it claims to run past the buffer. */
    uint32_t bad[32];
    for (int i = 0; i < 32; i++) bad[i] = buf[i];
    bad[3] = 0xffffffffu;                      /* GET_BOARD_REV value size = huge */
    const uint32_t *val; int nw;
    CHECK(mbox_find_tag(bad, 32, MBOX_TAG_GET_BOARD_REV, &val, &nw) == -1,
          "an over-long tag value is rejected, not over-read");

    /* Builder overflow: a tiny buffer cannot hold a big tag. */
    uint32_t small[6];
    mbox_builder_t sb;
    mbox_init(&sb, small, 6);
    CHECK(mbox_add_tag(&sb, MBOX_TAG_SET_CLOCK_RATE, NULL, 0, 8) == -1,
          "a tag larger than the buffer fails to append");
    CHECK(mbox_finish(&sb) == -1, "finishing an overflowed build fails");
}

int main(void)
{
    printf("=== Tessera VideoCore mailbox tests (M10, #105) ===\n");
    test_build();
    test_find_and_response();
    test_malformed_and_overflow();
    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
