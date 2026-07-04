/* drivers/mailbox.c - VideoCore property mailbox (Theme M10, issue #105).
 * See mailbox.h. */

#include "mailbox.h"

void mbox_init(mbox_builder_t *b, uint32_t *buf, int cap)
{
    b->buf = buf;
    b->cap = cap;
    b->ok  = 1;
    /* Reserve word 0 (total size) and word 1 (request/response code). */
    if (cap < 2) { b->ok = 0; b->n = 0; return; }
    buf[0] = 0;
    buf[1] = MBOX_CODE_REQUEST;
    b->n = 2;
}

int mbox_add_tag(mbox_builder_t *b, uint32_t tag, const uint32_t *req,
                 int req_words, int resp_words)
{
    if (!b->ok)
        return -1;
    if (req_words < 0)  req_words = 0;
    if (resp_words < 0) resp_words = 0;
    int val_words = req_words > resp_words ? req_words : resp_words;
    int need = 3 + val_words;            /* tag + size + code + value buffer */
    if (b->n + need > b->cap) { b->ok = 0; return -1; }

    int at = b->n;
    b->buf[at]     = tag;
    b->buf[at + 1] = (uint32_t)val_words * 4u;   /* value buffer size in bytes */
    b->buf[at + 2] = MBOX_CODE_REQUEST;          /* req/resp word (0 = request) */
    int value = at + 3;
    for (int i = 0; i < val_words; i++)
        b->buf[value + i] = (i < req_words && req) ? req[i] : 0u;
    b->n += need;
    return value;
}

int mbox_finish(mbox_builder_t *b)
{
    if (!b->ok)
        return -1;
    if (b->n + 1 > b->cap) { b->ok = 0; return -1; }
    b->buf[b->n++] = MBOX_TAG_END;
    b->buf[0] = (uint32_t)b->n * 4u;             /* total size in bytes */
    b->buf[1] = MBOX_CODE_REQUEST;
    return b->n;
}

int mbox_response_ok(const uint32_t *buf)
{
    return buf[1] == MBOX_CODE_RESP_OK;
}

int mbox_find_tag(const uint32_t *buf, int cap, uint32_t tag,
                  const uint32_t **value, int *nwords)
{
    if (cap < 3)
        return -1;
    /* The total size (bytes) also bounds the walk. */
    int total_words = (int)(buf[0] / 4u);
    if (total_words > cap) total_words = cap;

    int i = 2;                                   /* first tag after the header */
    while (i + 3 <= total_words) {
        uint32_t t = buf[i];
        if (t == MBOX_TAG_END)
            break;
        uint32_t val_bytes = buf[i + 1];
        int val_words = (int)(val_bytes / 4u);
        if (val_words < 0 || i + 3 + val_words > total_words)
            return -1;                           /* malformed / runs past end */
        if (t == tag) {
            if (value)  *value  = &buf[i + 3];
            if (nwords) *nwords = val_words;
            return 0;
        }
        i += 3 + val_words;
    }
    return -1;
}

int mbox_tag_has_response(const uint32_t *buf, const uint32_t *value)
{
    (void)buf;
    /* The tag's req/resp word sits immediately before the value buffer. */
    return (value[-1] & MBOX_TAG_RESP_BIT) != 0;
}

/* ---- hardware doorbell (kernel only) ------------------------------------- */

#define MBOX_BASE     0xFE00B880UL
#define MBOX_READ     (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS   (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE    (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_FULL     0x80000000u
#define MBOX_EMPTY    0x40000000u

void mbox_call(uint32_t channel, volatile uint32_t *buf)
{
    /* The message address must be 16-byte aligned; the low 4 bits carry the
     * channel.  (The VideoCore sees the bus-address alias; boards that need the
     * 0xC0000000 alias apply it before calling.) */
    uint32_t addr = (uint32_t)(uintptr_t)buf & ~0xFu;
    uint32_t msg  = addr | (channel & 0xFu);

    while (MBOX_STATUS & MBOX_FULL) { }
    MBOX_WRITE = msg;
    for (;;) {
        while (MBOX_STATUS & MBOX_EMPTY) { }
        if (MBOX_READ == msg)
            return;                              /* our reply on this channel */
    }
}
