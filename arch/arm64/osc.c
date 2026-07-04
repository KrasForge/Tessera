/* arch/arm64/osc.c - OSC codec + remote-editor dispatch (Theme E, issue #123).
 * See osc.h. */

#include "osc.h"

/* ---- big-endian 32-bit access (OSC is big-endian) ------------------------ */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Length of a NUL-terminated string rounded up to the next multiple of 4 (OSC
 * pads every string with at least one NUL).  Returns 0 if no NUL is found
 * within `max` bytes. */
static int padded_strlen(const char *s, int max)
{
    int i = 0;
    while (i < max && s[i] != '\0') i++;
    if (i >= max) return 0;                 /* unterminated within bounds */
    return (i + 4) & ~3;                    /* include the NUL, pad to 4  */
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- encode -------------------------------------------------------------- */

/* Append a string with its NUL and 4-byte padding.  Returns the new offset, or
 * -1 on overflow. */
static int emit_str(uint8_t *buf, int cap, int off, const char *s)
{
    int i = 0;
    while (s[i]) i++;
    int total = (i + 4) & ~3;               /* string + >=1 NUL, padded */
    if (off + total > cap) return -1;
    for (int k = 0; k < i; k++) buf[off + k] = (uint8_t)s[k];
    for (int k = i; k < total; k++) buf[off + k] = 0;
    return off + total;
}

int osc_encode(uint8_t *buf, int cap, const char *address,
               const osc_arg_t *args, int n_args)
{
    if (n_args < 0 || n_args > OSC_MAX_ARGS)
        return -1;

    int off = emit_str(buf, cap, 0, address);
    if (off < 0) return -1;

    /* Type-tag string: ',' followed by one char per arg. */
    char tags[OSC_MAX_ARGS + 2];
    tags[0] = ',';
    for (int a = 0; a < n_args; a++) {
        char t = args[a].type;
        if (t != OSC_INT32 && t != OSC_FLOAT32 && t != OSC_STRING)
            return -1;
        tags[a + 1] = t;
    }
    tags[n_args + 1] = '\0';
    off = emit_str(buf, cap, off, tags);
    if (off < 0) return -1;

    for (int a = 0; a < n_args; a++) {
        switch (args[a].type) {
        case OSC_INT32:
            if (off + 4 > cap) return -1;
            put_be32(buf + off, (uint32_t)args[a].v.i);
            off += 4;
            break;
        case OSC_FLOAT32:
            if (off + 4 > cap) return -1;
            put_be32(buf + off, args[a].v.f);   /* bit pattern, verbatim */
            off += 4;
            break;
        case OSC_STRING:
            off = emit_str(buf, cap, off, args[a].v.s);
            if (off < 0) return -1;
            break;
        default:
            return -1;
        }
    }
    return off;
}

/* ---- parse --------------------------------------------------------------- */

int osc_parse(const uint8_t *buf, int len, osc_message_t *msg)
{
    if (len <= 0 || (len & 3))               /* OSC packets are 4-byte aligned */
        return -1;

    const char *addr = (const char *)buf;
    if (addr[0] != '/')                      /* address patterns start with '/' */
        return -1;
    int off = padded_strlen(addr, len);
    if (off == 0) return -1;
    msg->address = addr;
    msg->n_args  = 0;

    if (off == len)                          /* address only, no type tags */
        return 0;

    const char *tags = (const char *)(buf + off);
    if (tags[0] != ',')                      /* type-tag string is mandatory here */
        return -1;
    int tstep = padded_strlen(tags, len - off);
    if (tstep == 0) return -1;
    off += tstep;

    for (int a = 1; tags[a] != '\0'; a++) {
        if (msg->n_args >= OSC_MAX_ARGS)
            return -1;                       /* too many args */
        osc_arg_t *arg = &msg->args[msg->n_args];
        arg->type = tags[a];
        switch (tags[a]) {
        case OSC_INT32:
            if (off + 4 > len) return -1;
            arg->v.i = (int32_t)get_be32(buf + off);
            off += 4;
            break;
        case OSC_FLOAT32:
            if (off + 4 > len) return -1;
            arg->v.f = get_be32(buf + off);  /* keep the bit pattern */
            off += 4;
            break;
        case OSC_STRING: {
            const char *s = (const char *)(buf + off);
            int step = padded_strlen(s, len - off);
            if (step == 0) return -1;
            arg->v.s = s;
            off += step;
            break;
        }
        default:
            return -1;                       /* unsupported type tag */
        }
        msg->n_args++;
    }
    return 0;
}

/* ---- editor dispatch ----------------------------------------------------- */

/* Do the argument types match a tag pattern like "iii" or "s"? */
static int args_match(const osc_message_t *m, const char *pat)
{
    int n = 0;
    for (; pat[n]; n++) {
        if (n >= m->n_args || m->args[n].type != pat[n])
            return 0;
    }
    return n == m->n_args;
}

int osc_editor_dispatch(const osc_message_t *msg, osc_cmd_t *cmd)
{
    cmd->type = OSC_CMD_NONE;
    const char *a = msg->address;

    if (str_eq(a, "/tessera/param") && args_match(msg, "iii")) {
        cmd->type       = OSC_CMD_SET_PARAM;
        cmd->plugin     = msg->args[0].v.i;
        cmd->id         = msg->args[1].v.i;
        cmd->value_bits = (uint32_t)msg->args[2].v.i;
        return 1;
    }
    /* /tessera/param also accepts a float value (,iif): same param, bit pattern. */
    if (str_eq(a, "/tessera/param") && args_match(msg, "iif")) {
        cmd->type       = OSC_CMD_SET_PARAM;
        cmd->plugin     = msg->args[0].v.i;
        cmd->id         = msg->args[1].v.i;
        cmd->value_bits = msg->args[2].v.f;
        return 1;
    }
    if (str_eq(a, "/tessera/connect") && args_match(msg, "ii")) {
        cmd->type = OSC_CMD_CONNECT;
        cmd->src  = msg->args[0].v.i;
        cmd->dst  = msg->args[1].v.i;
        return 1;
    }
    if (str_eq(a, "/tessera/disconnect") && args_match(msg, "ii")) {
        cmd->type = OSC_CMD_DISCONNECT;
        cmd->src  = msg->args[0].v.i;
        cmd->dst  = msg->args[1].v.i;
        return 1;
    }
    if (str_eq(a, "/tessera/load") && args_match(msg, "s")) {
        cmd->type = OSC_CMD_LOAD;
        cmd->path = msg->args[0].v.s;
        return 1;
    }
    if (str_eq(a, "/tessera/save") && args_match(msg, "s")) {
        cmd->type = OSC_CMD_SAVE;
        cmd->path = msg->args[0].v.s;
        return 1;
    }
    if (str_eq(a, "/tessera/ping") && msg->n_args == 0) {
        cmd->type = OSC_CMD_PING;
        return 1;
    }
    return 0;
}
