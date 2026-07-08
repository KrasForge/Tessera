/* sdk/lib/tessera_pconv.c - uniform-partitioned FFT convolution for the
 * Tessera SDK (Theme M18, issue #185).
 *
 * The time-domain engine (tessera_conv.c) costs ir_len multiply-adds per
 * SAMPLE - deliberately heavy.  This is the algorithm real products use for
 * long IRs (cabinets, rooms): split the impulse response into block-sized
 * partitions, transform each once at load, and per BLOCK do one forward FFT,
 * P complex multiply-accumulates over the bins, and one inverse FFT
 * (uniform-partitioned overlap-save).  Per-block cost is bounded and
 * independent of block position: O(B log B + P*B) for a B-sample block and a
 * P-partition IR, instead of O(B * ir_len).
 *
 * Same real-time contract as the rest of the SDK: the caller owns every
 * buffer (IR spectra, input-spectrum ring, accumulator, work frames, twiddle
 * table), nothing allocates, and all transforms happen on caller memory.  The
 * IR is transformed in tessera_pconv_init - call it at load time, off the
 * audio path.  No libc, no libm.
 */

#include "tessera.h"

uint32_t tessera_pconv_parts(uint32_t block, uint32_t ir_len)
{
    if (block == 0u)
        return 0u;
    return (ir_len + block - 1u) / block;
}

int tessera_pconv_init(tessera_pconv_t *pc, uint32_t block,
                       const float *ir, uint32_t ir_len,
                       const tessera_cpx_t *tw,
                       tessera_cpx_t *h_spec, tessera_cpx_t *x_spec,
                       tessera_cpx_t *acc, float *work)
{
    if (!pc || !ir || !tw || !h_spec || !x_spec || !acc || !work)
        return -1;
    if (block < 4u || (block & (block - 1u)) != 0u || ir_len == 0u)
        return -1;

    pc->block  = block;
    pc->fft    = 2u * block;
    pc->bins   = block + 1u;             /* rfft of 2B samples: bins 0..B */
    pc->parts  = tessera_pconv_parts(block, ir_len);
    pc->slot   = 0;
    pc->tw     = tw;
    pc->h_spec = h_spec;
    pc->x_spec = x_spec;
    pc->acc    = acc;
    pc->work   = work;

    /* Transform each zero-padded partition of the IR (load time, not the
     * audio path).  The scratch half of `work` assembles the padded frame. */
    float *frame = work + pc->fft;       /* scratch half */
    for (uint32_t p = 0; p < pc->parts; p++) {
        for (uint32_t i = 0; i < pc->fft; i++)
            frame[i] = 0.0f;
        uint32_t base = p * block;
        uint32_t n = ir_len - base < block ? ir_len - base : block;
        for (uint32_t i = 0; i < n; i++)
            frame[i] = ir[base + i];
        tessera_rfft(frame, h_spec + p * pc->bins, tw, pc->fft);
    }

    tessera_pconv_reset(pc);
    return 0;
}

void tessera_pconv_reset(tessera_pconv_t *pc)
{
    pc->slot = 0;
    for (uint32_t i = 0; i < pc->parts * pc->bins; i++) {
        pc->x_spec[i].re = 0.0f;
        pc->x_spec[i].im = 0.0f;
    }
    for (uint32_t i = 0; i < pc->fft; i++)
        pc->work[i] = 0.0f;              /* persistent frame: silence history */
}

void tessera_pconv_process(tessera_pconv_t *pc, const float *in, float *out)
{
    uint32_t B = pc->block, F = pc->fft, K = pc->bins, P = pc->parts;
    float *frame   = pc->work;           /* persistent: last F input samples  */
    float *scratch = pc->work + F;       /* per-call inverse-transform target */

    /* Slide the frame: previous block moves to the front, the new block
     * fills the back, so frame always holds the last 2B input samples. */
    for (uint32_t i = 0; i < B; i++)
        frame[i] = frame[B + i];
    for (uint32_t i = 0; i < B; i++)
        frame[B + i] = in[i];

    /* Advance the spectrum ring and transform the current frame into it. */
    pc->slot = (pc->slot + 1u) % P;
    tessera_cpx_t *xcur = pc->x_spec + pc->slot * K;
    tessera_rfft(frame, xcur, pc->tw, F);

    /* Y[k] = sum_p X_{p blocks ago}[k] * H_p[k]  (bin-wise complex MAC). */
    for (uint32_t k = 0; k < K; k++) {
        pc->acc[k].re = 0.0f;
        pc->acc[k].im = 0.0f;
    }
    for (uint32_t p = 0; p < P; p++) {
        const tessera_cpx_t *x = pc->x_spec + ((pc->slot + P - p) % P) * K;
        const tessera_cpx_t *h = pc->h_spec + p * K;
        for (uint32_t k = 0; k < K; k++) {
            pc->acc[k].re += x[k].re * h[k].re - x[k].im * h[k].im;
            pc->acc[k].im += x[k].re * h[k].im + x[k].im * h[k].re;
        }
    }

    /* Inverse transform; overlap-save keeps the last B samples (the first B
     * carry the circular wrap-around and are discarded). */
    tessera_irfft(pc->acc, scratch, pc->tw, F);
    for (uint32_t i = 0; i < B; i++)
        out[i] = scratch[B + i];
}
