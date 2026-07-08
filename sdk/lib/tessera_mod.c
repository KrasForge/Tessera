/* sdk/lib/tessera_mod.c - modulation matrix (Theme M19, issue #188).
 *
 * The SDK has modulation SOURCES (LFOs, ADSRs, envelope followers) and ways
 * to SET parameters (the param queue, MIDI learn, morphing) - this is the
 * routing fabric between them, the thing that turns "set a parameter" into a
 * living instrument.  A table of routes {source, destination, depth, curve};
 * per block, every enabled route's contribution is summed onto its
 * destination and the result clamped into the destination's range:
 *
 *     value[d] = clamp(base[d] + sum_r depth_r * curve_r(src[r]), lo, hi)
 *
 * Conventions:
 *   - Source values are published by the caller each block (control rate,
 *     not per sample): unipolar sources in [0, 1] (envelopes, pedals,
 *     pressure), bipolar sources in [-1, 1] (LFOs, pitch bend).  The matrix
 *     does not care which - depth is signed, so any polarity routes both
 *     ways.
 *   - depth scales the source into destination units (a cutoff route with
 *     depth 4000 swings +/-4000 Hz for a full-scale bipolar LFO).
 *   - curve is applied to the source value first: LIN passes it through, EXP
 *     is x*|x| (gentle near zero, steep at the ends - natural for cutoff and
 *     level), INV negates.
 *
 * Pure C over caller-owned tables: no allocation, no libc, no libm, bounded
 * work (n_routes adds per evaluate).  Host-tested: make test-arm-modmatrix.
 */

#include "tessera.h"

void tessera_mod_init(tessera_mod_t *m,
                      tessera_mod_route_t *routes, uint32_t max_routes,
                      float *sources, uint32_t n_sources,
                      tessera_mod_dest_t *dests, uint32_t n_dests)
{
    m->routes     = routes;
    m->max_routes = max_routes;
    m->n_routes   = 0;
    m->sources    = sources;
    m->n_sources  = n_sources;
    m->dests      = dests;
    m->n_dests    = n_dests;
    for (uint32_t s = 0; s < n_sources; s++)
        sources[s] = 0.0f;
    for (uint32_t d = 0; d < n_dests; d++)
        dests[d].value = dests[d].base;
}

void tessera_mod_dest_setup(tessera_mod_t *m, uint32_t dst,
                            float base, float lo, float hi)
{
    if (dst >= m->n_dests)
        return;
    m->dests[dst].base  = base;
    m->dests[dst].lo    = lo;
    m->dests[dst].hi    = hi;
    m->dests[dst].value = tessera_clampf(base, lo, hi);
}

int tessera_mod_route(tessera_mod_t *m, uint32_t src, uint32_t dst,
                      float depth, uint8_t curve)
{
    if (src >= m->n_sources || dst >= m->n_dests || curve > TESSERA_MOD_INV)
        return -1;
    /* Reuse a disabled slot before growing the table. */
    uint32_t slot = m->n_routes;
    for (uint32_t r = 0; r < m->n_routes; r++)
        if (!m->routes[r].on) { slot = r; break; }
    if (slot == m->n_routes) {
        if (m->n_routes >= m->max_routes)
            return -1;
        m->n_routes++;
    }
    m->routes[slot].src   = (uint16_t)src;
    m->routes[slot].dst   = (uint16_t)dst;
    m->routes[slot].depth = depth;
    m->routes[slot].curve = curve;
    m->routes[slot].on    = 1;
    return (int)slot;
}

void tessera_mod_unroute(tessera_mod_t *m, int route)
{
    if (route >= 0 && (uint32_t)route < m->n_routes)
        m->routes[route].on = 0;
}

void tessera_mod_set_source(tessera_mod_t *m, uint32_t src, float value)
{
    if (src < m->n_sources)
        m->sources[src] = value;
}

void tessera_mod_set_base(tessera_mod_t *m, uint32_t dst, float base)
{
    if (dst < m->n_dests)
        m->dests[dst].base = base;
}

static float curve_apply(uint8_t curve, float x)
{
    switch (curve) {
    case TESSERA_MOD_EXP: return x * (x < 0.0f ? -x : x);   /* x*|x| */
    case TESSERA_MOD_INV: return -x;
    default:              return x;
    }
}

void tessera_mod_eval(tessera_mod_t *m)
{
    for (uint32_t d = 0; d < m->n_dests; d++)
        m->dests[d].value = m->dests[d].base;

    for (uint32_t r = 0; r < m->n_routes; r++) {
        const tessera_mod_route_t *rt = &m->routes[r];
        if (!rt->on)
            continue;
        m->dests[rt->dst].value +=
            rt->depth * curve_apply(rt->curve, m->sources[rt->src]);
    }

    for (uint32_t d = 0; d < m->n_dests; d++)
        m->dests[d].value = tessera_clampf(m->dests[d].value,
                                           m->dests[d].lo, m->dests[d].hi);
}

float tessera_mod_value(const tessera_mod_t *m, uint32_t dst)
{
    return dst < m->n_dests ? m->dests[dst].value : 0.0f;
}
