/* arch/arm64/param_map.h - control-event to plugin-parameter mapping
 *                          (Issue #33, M7)
 *
 * The host side of live parameter control: it maps an incoming control event
 * (a MIDI Control Change, or a CV value tagged as a CC) to a plugin parameter
 * id and a scaled float value, which it then enqueues on the plugin's lock-free
 * parameter queue (issue #30).  The plugin drains that queue at the top of each
 * process_block and applies the value, so a knob turn reaches the audio without
 * any lock or syscall on the audio path.
 *
 * This is host control logic and uses floating point, so it is header-only
 * (inline) and compiled only into floating-point-enabled code (the host tests
 * and the FP virt harness) - never into the -mgeneral-regs-only kernel.
 */

#ifndef ARM64_PARAM_MAP_H
#define ARM64_PARAM_MAP_H

#include "midi.h"
#include <stdint.h>

#define PARAM_MAP_MAX 16

typedef struct {
    uint8_t  cc;         /* MIDI CC number this binding listens to     */
    uint32_t param_id;   /* plugin parameter id it drives              */
    float    min;        /* value at CC 0                              */
    float    max;        /* value at CC 127                            */
} param_binding_t;

typedef struct {
    param_binding_t b[PARAM_MAP_MAX];
    int             n;
} param_map_t;

static inline void param_map_init(param_map_t *m) { m->n = 0; }

/* Bind CC `cc` to `param_id`, scaling CC 0..127 onto [min, max].  Returns 0 on
 * success, -1 if the map is full. */
static inline int param_map_bind(param_map_t *m, uint8_t cc, uint32_t param_id,
                                 float min, float max)
{
    if (m->n >= PARAM_MAP_MAX)
        return -1;
    m->b[m->n].cc       = cc;
    m->b[m->n].param_id = param_id;
    m->b[m->n].min      = min;
    m->b[m->n].max      = max;
    m->n++;
    return 0;
}

/* If `ev` is a CC bound in the map, write the target parameter id and scaled
 * value and return 1; otherwise return 0. */
static inline int param_map_event(const param_map_t *m, const midi_event_t *ev,
                                  uint32_t *param_id, float *value)
{
    if (ev->type != MIDI_CC)
        return 0;
    for (int i = 0; i < m->n; i++) {
        if (m->b[i].cc == ev->data1) {
            float t = (float)ev->data2 / 127.0f;          /* 0..1 */
            *param_id = m->b[i].param_id;
            *value    = m->b[i].min + t * (m->b[i].max - m->b[i].min);
            return 1;
        }
    }
    return 0;
}

#endif /* ARM64_PARAM_MAP_H */
