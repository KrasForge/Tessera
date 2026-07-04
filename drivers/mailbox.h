/* drivers/mailbox.h - VideoCore property mailbox (Theme M10, issue #105)
 *
 * The BCM2711's ARM cores talk to the VideoCore firmware through a mailbox: the
 * ARM writes a 16-byte-aligned message buffer address (OR the channel number)
 * to the mailbox and the firmware fills the buffer with responses.  The property
 * channel (8) carries a tag list - each tag a request like "get board revision"
 * or "set the PCM clock to 48 kHz*256" - which is how the kernel learns the
 * board revision and configures the UART / I2S clocks (issue #105).
 *
 * The message *format* is pure serialisation, independent of the hardware
 * doorbell, so it is built and parsed here and host-tested (make test-arm-mailbox).
 * The single MMIO doorbell (mbox_call) is a thin hardware wrapper compiled only
 * into the kernel.
 */

#ifndef TESSERA_MAILBOX_H
#define TESSERA_MAILBOX_H

#include <stdint.h>

#define MBOX_CH_PROP        8u

/* Message-level request/response codes (word 1 of the buffer). */
#define MBOX_CODE_REQUEST   0x00000000u
#define MBOX_CODE_RESP_OK   0x80000000u
#define MBOX_CODE_RESP_ERR  0x80000001u

/* Per-tag response marker: bit 31 set in the tag's req/resp word. */
#define MBOX_TAG_RESP_BIT   0x80000000u

/* A handful of property tags the kernel uses. */
#define MBOX_TAG_GET_FIRMWARE   0x00000001u
#define MBOX_TAG_GET_BOARD_REV  0x00010002u
#define MBOX_TAG_GET_CLOCK_RATE 0x00030002u
#define MBOX_TAG_SET_CLOCK_RATE 0x00038002u
#define MBOX_TAG_END            0x00000000u

/* Clock ids for GET/SET_CLOCK_RATE. */
#define MBOX_CLOCK_UART  2u
#define MBOX_CLOCK_PCM   5u

/* Incremental builder over a caller-supplied uint32 buffer (must be 16-byte
 * aligned for the real doorbell; the builder itself does not require it). */
typedef struct {
    uint32_t *buf;
    int       cap;    /* capacity in words */
    int       n;      /* words written so far */
    int       ok;     /* 0 once an append overflowed */
} mbox_builder_t;

/* Begin a property message in `buf` (`cap` words).  Reserves the size and
 * request-code header words. */
void mbox_init(mbox_builder_t *b, uint32_t *buf, int cap);

/* Append a tag: `tag`, `req` request words (may be NULL when req_words==0), and
 * a value buffer sized to hold max(req_words, resp_words) words.  Returns the
 * word index of the tag's value buffer (for reading the response after the
 * call), or -1 on overflow. */
int  mbox_add_tag(mbox_builder_t *b, uint32_t tag, const uint32_t *req,
                  int req_words, int resp_words);

/* Write the end tag and patch the total byte size into word 0.  Returns the
 * total message length in words, or -1 if the build overflowed. */
int  mbox_finish(mbox_builder_t *b);

/* Whether a completed response buffer reports overall success (word 1). */
int  mbox_response_ok(const uint32_t *buf);

/* Locate a tag's value buffer in a (request or response) message.  On success
 * returns 0 and sets `*value` to the value words and `*nwords` to the value
 * buffer size in words; returns -1 if the tag is absent or the buffer is
 * malformed. */
int  mbox_find_tag(const uint32_t *buf, int cap, uint32_t tag,
                   const uint32_t **value, int *nwords);

/* Whether a located tag carries a firmware *response* (its req/resp word has bit
 * 31 set).  `value` is the pointer returned by mbox_find_tag. */
int  mbox_tag_has_response(const uint32_t *buf, const uint32_t *value);

/* ---- hardware doorbell (kernel only) ------------------------------------- */
/* Send `buf` on `channel` and spin until the firmware answers.  Defined in
 * mailbox.c behind the MMIO; not exercised by the host tests. */
void mbox_call(uint32_t channel, volatile uint32_t *buf);

#endif /* TESSERA_MAILBOX_H */
