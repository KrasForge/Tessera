/* arch/arm64/blackbox.c - crash black-box: post-mortem across a reboot
 * (Theme A: reliability) */

#include "blackbox.h"

/* ---- little-endian helpers ---------------------------------------------- */

static void put_u32(uint8_t *b, size_t *off, uint32_t v)
{
    b[*off + 0] = (uint8_t)(v      );
    b[*off + 1] = (uint8_t)(v >>  8);
    b[*off + 2] = (uint8_t)(v >> 16);
    b[*off + 3] = (uint8_t)(v >> 24);
    *off += 4;
}
static uint32_t get_u32(const uint8_t *b, size_t off)
{
    return (uint32_t)b[off]        | ((uint32_t)b[off + 1] <<  8)
         | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* FNV-1a over a byte range - catches bit flips and transpositions a plain sum
 * would miss, so a corrupt store can never yield a plausible post-mortem. */
static uint32_t fnv1a(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

/* Fixed header: 9 u32 fields + the fixed-size name. */
#define BB_HDR_BYTES (9u * 4u + BB_NAME_MAX)

/* ---- recorder ----------------------------------------------------------- */

void bb_init(blackbox_t *bb, uint32_t block_words)
{
    if (block_words > BB_MAX_BLOCK_WORDS)
        block_words = BB_MAX_BLOCK_WORDS;
    bb->block_words = block_words;
    bb->head = bb->count = bb->total = 0u;
    bb->captured = 0u;
    bb->fault_pid = 0u;
    bb->fault_cause = BB_CAUSE_NONE;
    bb->fault_block = 0u;
    for (uint32_t i = 0; i < BB_NAME_MAX; i++)
        bb->fault_name[i] = '\0';
}

void bb_record(blackbox_t *bb, const uint32_t *block)
{
    uint32_t *slot = bb->ring[bb->head];
    for (uint32_t w = 0; w < bb->block_words; w++)
        slot[w] = block[w];
    bb->head = (bb->head + 1u) % BB_BLOCKS;
    if (bb->count < BB_BLOCKS)
        bb->count++;
    bb->total++;
}

int bb_chrono(const blackbox_t *bb, uint32_t i, uint32_t *out)
{
    if (i >= bb->count)
        return 0;
    uint32_t start = (bb->head + BB_BLOCKS - bb->count) % BB_BLOCKS;
    const uint32_t *slot = bb->ring[(start + i) % BB_BLOCKS];
    for (uint32_t w = 0; w < bb->block_words; w++)
        out[w] = slot[w];
    return 1;
}

void bb_capture(blackbox_t *bb, uint32_t pid, const char *name, bb_cause_t cause)
{
    if (bb->captured)
        return;                 /* the first (real) crash wins */
    bb->captured    = 1u;
    bb->fault_pid   = pid;
    bb->fault_cause = (uint32_t)cause;
    bb->fault_block = bb->total;
    uint32_t i = 0;
    if (name)
        for (; i < BB_NAME_MAX - 1u && name[i]; i++)
            bb->fault_name[i] = name[i];
    for (; i < BB_NAME_MAX; i++)
        bb->fault_name[i] = '\0';
}

/* ---- serialisation ------------------------------------------------------ */

size_t bb_serialized_size(const blackbox_t *bb)
{
    return (size_t)BB_HDR_BYTES
         + (size_t)bb->count * bb->block_words * 4u
         + 4u;                  /* trailing checksum */
}

size_t bb_serialize(const blackbox_t *bb, uint8_t *buf, size_t cap)
{
    size_t need = bb_serialized_size(bb);
    if (cap < need)
        return 0;

    size_t off = 0;
    put_u32(buf, &off, BB_MAGIC);
    put_u32(buf, &off, BB_VERSION);
    put_u32(buf, &off, bb->block_words);
    put_u32(buf, &off, bb->count);
    put_u32(buf, &off, bb->total);
    put_u32(buf, &off, bb->fault_pid);
    put_u32(buf, &off, bb->fault_cause);
    put_u32(buf, &off, bb->fault_block);
    put_u32(buf, &off, bb->captured);
    for (uint32_t i = 0; i < BB_NAME_MAX; i++)
        buf[off++] = (uint8_t)bb->fault_name[i];

    /* blocks in chronological order (oldest kept first) */
    uint32_t start = (bb->head + BB_BLOCKS - bb->count) % BB_BLOCKS;
    for (uint32_t i = 0; i < bb->count; i++) {
        const uint32_t *slot = bb->ring[(start + i) % BB_BLOCKS];
        for (uint32_t w = 0; w < bb->block_words; w++)
            put_u32(buf, &off, slot[w]);
    }

    put_u32(buf, &off, fnv1a(buf, off));
    return off;
}

int bb_parse(const uint8_t *buf, size_t len, blackbox_t *out)
{
    if (len < (size_t)BB_HDR_BYTES + 4u)
        return 0;
    if (get_u32(buf, 0) != BB_MAGIC)      return 0;
    if (get_u32(buf, 4) != BB_VERSION)    return 0;

    uint32_t block_words = get_u32(buf, 8);
    uint32_t count       = get_u32(buf, 12);
    if (block_words == 0u || block_words > BB_MAX_BLOCK_WORDS) return 0;
    if (count > BB_BLOCKS)                                     return 0;

    size_t expect = (size_t)BB_HDR_BYTES + (size_t)count * block_words * 4u + 4u;
    if (len != expect)
        return 0;
    if (fnv1a(buf, expect - 4u) != get_u32(buf, expect - 4u))
        return 0;

    bb_init(out, block_words);
    out->total       = get_u32(buf, 16);
    out->fault_pid   = get_u32(buf, 20);
    out->fault_cause = get_u32(buf, 24);
    out->fault_block = get_u32(buf, 28);
    out->captured    = get_u32(buf, 32);
    size_t off = 36;
    for (uint32_t i = 0; i < BB_NAME_MAX; i++)
        out->fault_name[i] = (char)buf[off++];
    out->fault_name[BB_NAME_MAX - 1u] = '\0';

    /* Rebuild the ring normalised: chronological in slots 0..count-1. */
    out->count = count;
    out->head  = count % BB_BLOCKS;
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t w = 0; w < block_words; w++)
            out->ring[i][w] = get_u32(buf, off + (size_t)(i * block_words + w) * 4u);

    return 1;
}
