/* sdk/lib/tessera_conv.c - impulse-response (FIR) convolution for the Tessera
 * SDK (Theme B, issue #112).
 *
 * A time-domain overlap FIR: the engine keeps a ring of the most recent input
 * samples and, per output sample, sums the impulse response against that
 * history.  This is deliberately the "heavy" building block - a cabinet /
 * room-simulator plugin convolving against a few-thousand-tap IR spends real
 * CPU every block, so it exercises the M12 CPU budget and the process isolation
 * under genuine load rather than a toy workload.
 *
 * Real-time safe: no libc, no allocation, no unbounded per-sample work (the cost
 * is exactly ir_len multiply-adds per sample, known up front).  The caller
 * supplies both the impulse response and the history ring, so the SDK never
 * allocates.
 */

#include "tessera.h"

void tessera_conv_init(tessera_conv_t *c, const float *ir, uint32_t ir_len,
                       float *hist, uint32_t hist_len)
{
    c->ir       = ir;
    c->ir_len   = ir_len;
    c->hist     = hist;
    c->hist_len = hist_len;
    c->w        = 0;
    for (uint32_t i = 0; i < hist_len; i++) hist[i] = 0.0f;
}

float tessera_conv(tessera_conv_t *c, float x)
{
    /* Push the newest sample; w points one past the most-recent write. */
    c->hist[c->w] = x;
    c->w = (c->w + 1u) % c->hist_len;

    /* y[n] = sum_k ir[k] * x[n-k].  ir[0] multiplies the newest sample (at
     * w-1), ir[1] the one before it, and so on. */
    uint32_t taps = c->ir_len <= c->hist_len ? c->ir_len : c->hist_len;
    float    acc  = 0.0f;
    uint32_t idx  = (c->w + c->hist_len - 1u) % c->hist_len;   /* newest */
    for (uint32_t k = 0; k < taps; k++) {
        acc += c->ir[k] * c->hist[idx];
        idx = idx == 0u ? c->hist_len - 1u : idx - 1u;
    }
    return acc;
}

void tessera_conv_reset(tessera_conv_t *c)
{
    c->w = 0;
    for (uint32_t i = 0; i < c->hist_len; i++) c->hist[i] = 0.0f;
}

float tessera_conv_normgain(const float *ir, uint32_t ir_len)
{
    /* Sum of |taps|: the worst-case (DC) gain of the IR, so a caller can
     * pre-scale a cabinet IR to keep the convolved signal from clipping. */
    float sum = 0.0f;
    for (uint32_t i = 0; i < ir_len; i++)
        sum += ir[i] < 0.0f ? -ir[i] : ir[i];
    return sum;
}
