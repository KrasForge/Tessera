/* arch/arm64/ctlmap.h - control-surface mapping with MIDI-learn (Theme E,
 * issue #120)
 *
 * The pedal front panel: footswitches, rotary encoders, an expression pedal,
 * and MIDI Control Change messages, all mapped to plugin parameters through one
 * table.  It is the integer, real-time-safe sibling of param_map.h (which is
 * float and host-only): every value is scaled in fixed integer range, so this
 * can live on the -mgeneral-regs-only audio path if a mapping is applied there.
 *
 * MIDI-learn: arm the map for a target parameter and the next control event to
 * arrive binds itself to that parameter - the standard "wiggle a knob to
 * assign" workflow - with no need to know the control's id in advance.
 *
 * Pure and host-tested (make test-arm-ctlmap); no allocation, no libc, no FP.
 */

#ifndef ARM64_CTLMAP_H
#define ARM64_CTLMAP_H

#include <stdint.h>

#define CTLMAP_MAX 32

/* Where a control value comes from.  Continuous sources (encoder, expression,
 * MIDI CC) carry a 0..127 position; a footswitch carries 0 (up) or 127 (down). */
typedef enum {
    CTL_SRC_NONE = 0,
    CTL_SRC_FOOTSW,     /* a momentary or latching footswitch */
    CTL_SRC_ENCODER,    /* a rotary encoder (absolute position 0..127) */
    CTL_SRC_EXPR,       /* an expression pedal (0..127) */
    CTL_SRC_MIDI_CC,    /* an incoming MIDI Control Change */
} ctl_src_type_t;

/* Identifies one physical/virtual control: e.g. footswitch #2, or MIDI CC 74. */
typedef struct {
    ctl_src_type_t type;
    uint16_t       id;
} ctl_source_t;

/* Footswitch behaviour. */
typedef enum {
    CTL_MODE_CONTINUOUS = 0, /* scale 0..127 onto [out_min,out_max]      */
    CTL_MODE_MOMENTARY,      /* down -> out_max, up -> out_min           */
    CTL_MODE_TOGGLE,         /* each press flips between out_min/out_max  */
} ctl_mode_t;

typedef struct {
    int          used;
    ctl_source_t src;
    uint32_t     param_id;
    int32_t      out_min, out_max;
    ctl_mode_t   mode;
    int          toggle_state;   /* current side for CTL_MODE_TOGGLE */
} ctl_binding_t;

typedef struct {
    ctl_binding_t bindings[CTLMAP_MAX];
    int           n;
    /* MIDI-learn arming: when learn_active, the next fed control binds here. */
    int      learn_active;
    uint32_t learn_param;
    int32_t  learn_min, learn_max;
    ctl_mode_t learn_mode;
} ctlmap_t;

void ctlmap_init(ctlmap_t *m);

/* Bind `src` to `param_id`, scaling onto [out_min,out_max] with `mode`.  If the
 * source is already bound, the existing binding is replaced.  Returns the
 * binding index, or -1 if the table is full. */
int  ctlmap_bind(ctlmap_t *m, ctl_source_t src, uint32_t param_id,
                 int32_t out_min, int32_t out_max, ctl_mode_t mode);

/* Remove any binding for `src`.  Returns 1 if one was removed, else 0. */
int  ctlmap_unbind(ctlmap_t *m, ctl_source_t src);

/* Find the binding index for `src`, or -1. */
int  ctlmap_find(const ctlmap_t *m, ctl_source_t src);

/* Arm MIDI-learn: the next control fed to ctlmap_feed binds to `param_id` with
 * these settings (replacing any prior binding for that control). */
void ctlmap_learn_begin(ctlmap_t *m, uint32_t param_id,
                        int32_t out_min, int32_t out_max, ctl_mode_t mode);
void ctlmap_learn_cancel(ctlmap_t *m);
int  ctlmap_learn_pending(const ctlmap_t *m);

/* Feed a control event.  `raw` is the control's value (0..127; a footswitch
 * uses >=64 for "down").  If learn is armed, `src` is bound first.  If `src` is
 * bound, writes the target parameter id and scaled value and returns 1;
 * otherwise returns 0.  A latching footswitch only reports on the press edge. */
int  ctlmap_feed(ctlmap_t *m, ctl_source_t src, int32_t raw,
                 uint32_t *out_param, int32_t *out_value);

#endif /* ARM64_CTLMAP_H */
