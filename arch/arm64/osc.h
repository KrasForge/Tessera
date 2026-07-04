/* arch/arm64/osc.h - OSC codec + remote-editor dispatch (Theme E, issue #123)
 *
 * The desktop/remote editor: build patches from a laptop over USB serial or OSC.
 * This is the wire format - an Open Sound Control 1.0 message codec (address
 * pattern + type-tag string + arguments) - plus a thin dispatch that turns
 * editor messages like `/tessera/param ,iii <plugin> <id> <value>` into the same
 * parameter / graph edits the on-device shell performs.
 *
 * OSC carries 32-bit floats, but the kernel builds -mgeneral-regs-only (no FP),
 * so float arguments are handled as their raw 32-bit IEEE-754 *bit pattern*
 * (type 'f', stored in a uint32) - never as a float - exactly as the patch file
 * format does.  No arithmetic is performed on them here.
 *
 * The parser is fed untrusted bytes from an external tool, so every field is
 * bounds-checked against the buffer before use.  Pure, host-tested
 * (make test-arm-osc); no allocation, no libc, no FP.
 */

#ifndef ARM64_OSC_H
#define ARM64_OSC_H

#include <stdint.h>

#define OSC_MAX_ARGS 8

typedef enum {
    OSC_INT32   = 'i',
    OSC_FLOAT32 = 'f',   /* carried as a 32-bit bit pattern, not a float */
    OSC_STRING  = 's',
} osc_type_t;

typedef struct {
    char type;                 /* one of osc_type_t */
    union {
        int32_t  i;            /* OSC_INT32                    */
        uint32_t f;            /* OSC_FLOAT32 bit pattern      */
        const char *s;         /* OSC_STRING (into the buffer) */
    } v;
} osc_arg_t;

typedef struct {
    const char *address;       /* NUL-terminated, into the buffer */
    int         n_args;
    osc_arg_t   args[OSC_MAX_ARGS];
} osc_message_t;

/* Encode an OSC message into `buf` (capacity `cap`).  Returns the byte length
 * written (always a multiple of 4), or -1 on overflow / too many args / a bad
 * type tag. */
int osc_encode(uint8_t *buf, int cap, const char *address,
               const osc_arg_t *args, int n_args);

/* Parse an OSC message from `buf`/`len` into `msg` (pointers alias into `buf`).
 * Returns 0 on success, -1 on any malformed / out-of-bounds field.  At most
 * OSC_MAX_ARGS arguments are decoded; a message with more is rejected. */
int osc_parse(const uint8_t *buf, int len, osc_message_t *msg);

/* ---- remote-editor dispatch --------------------------------------------- */

typedef enum {
    OSC_CMD_NONE = 0,
    OSC_CMD_SET_PARAM,   /* /tessera/param   ,iii plugin id valuebits */
    OSC_CMD_CONNECT,     /* /tessera/connect ,ii  src dst             */
    OSC_CMD_DISCONNECT,  /* /tessera/disconnect ,ii src dst           */
    OSC_CMD_LOAD,        /* /tessera/load    ,s   path                */
    OSC_CMD_SAVE,        /* /tessera/save    ,s   path                */
    OSC_CMD_PING,        /* /tessera/ping                             */
} osc_cmd_type_t;

typedef struct {
    osc_cmd_type_t type;
    int32_t     plugin, id, src, dst;
    uint32_t    value_bits;    /* IEEE-754 bit pattern for a param value */
    const char *path;          /* for LOAD / SAVE */
} osc_cmd_t;

/* Interpret a parsed message as an editor command.  Returns 1 and fills `cmd`
 * for a recognised address with the right type tags, else 0. */
int osc_editor_dispatch(const osc_message_t *msg, osc_cmd_t *cmd);

#endif /* ARM64_OSC_H */
