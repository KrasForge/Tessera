/* arch/arm64/ctlmap.c - control-surface mapping with MIDI-learn (Theme E,
 * issue #120).  See ctlmap.h. */

#include "ctlmap.h"

void ctlmap_init(ctlmap_t *m)
{
    for (int i = 0; i < CTLMAP_MAX; i++)
        m->bindings[i].used = 0;
    m->n            = 0;
    m->learn_active = 0;
    m->learn_param  = 0;
}

static int src_eq(const ctl_source_t *a, const ctl_source_t *b)
{
    return a->type == b->type && a->id == b->id;
}

int ctlmap_find(const ctlmap_t *m, ctl_source_t src)
{
    for (int i = 0; i < CTLMAP_MAX; i++)
        if (m->bindings[i].used && src_eq(&m->bindings[i].src, &src))
            return i;
    return -1;
}

int ctlmap_bind(ctlmap_t *m, ctl_source_t src, uint32_t param_id,
                int32_t out_min, int32_t out_max, ctl_mode_t mode)
{
    if (src.type == CTL_SRC_NONE)
        return -1;

    /* Replace an existing binding for the same control in place. */
    int i = ctlmap_find(m, src);
    if (i < 0) {
        for (i = 0; i < CTLMAP_MAX; i++)
            if (!m->bindings[i].used)
                break;
        if (i == CTLMAP_MAX)
            return -1;              /* table full */
        m->bindings[i].used = 1;
        m->n++;
    }
    m->bindings[i].src          = src;
    m->bindings[i].param_id     = param_id;
    m->bindings[i].out_min      = out_min;
    m->bindings[i].out_max      = out_max;
    m->bindings[i].mode         = mode;
    m->bindings[i].toggle_state = 0;
    return i;
}

int ctlmap_unbind(ctlmap_t *m, ctl_source_t src)
{
    int i = ctlmap_find(m, src);
    if (i < 0)
        return 0;
    m->bindings[i].used = 0;
    m->n--;
    return 1;
}

void ctlmap_learn_begin(ctlmap_t *m, uint32_t param_id,
                        int32_t out_min, int32_t out_max, ctl_mode_t mode)
{
    m->learn_active = 1;
    m->learn_param  = param_id;
    m->learn_min    = out_min;
    m->learn_max    = out_max;
    m->learn_mode   = mode;
}

void ctlmap_learn_cancel(ctlmap_t *m) { m->learn_active = 0; }
int  ctlmap_learn_pending(const ctlmap_t *m) { return m->learn_active; }

/* Scale a 0..127 position onto [out_min,out_max], rounded to nearest. */
static int32_t scale_cont(int32_t raw, int32_t out_min, int32_t out_max)
{
    if (raw < 0)   raw = 0;
    if (raw > 127) raw = 127;
    int32_t span = out_max - out_min;
    /* Round to nearest: add half a step (guarding the sign of span). */
    int32_t half = span >= 0 ? 63 : -63;
    return out_min + (span * raw + half) / 127;
}

int ctlmap_feed(ctlmap_t *m, ctl_source_t src, int32_t raw,
                uint32_t *out_param, int32_t *out_value)
{
    if (src.type == CTL_SRC_NONE)
        return 0;

    /* MIDI-learn: bind this control to the armed parameter, then fall through
     * and report its first value immediately. */
    if (m->learn_active) {
        ctlmap_bind(m, src, m->learn_param, m->learn_min, m->learn_max,
                    m->learn_mode);
        m->learn_active = 0;
    }

    int i = ctlmap_find(m, src);
    if (i < 0)
        return 0;
    ctl_binding_t *b = &m->bindings[i];

    int down = raw >= 64;
    int32_t value;
    switch (b->mode) {
    case CTL_MODE_MOMENTARY:
        value = down ? b->out_max : b->out_min;
        break;
    case CTL_MODE_TOGGLE:
        if (!down)
            return 0;                       /* only act on the press edge */
        b->toggle_state = !b->toggle_state;
        value = b->toggle_state ? b->out_max : b->out_min;
        break;
    case CTL_MODE_CONTINUOUS:
    default:
        value = scale_cont(raw, b->out_min, b->out_max);
        break;
    }
    *out_param = b->param_id;
    *out_value = value;
    return 1;
}
