/* arch/arm64/multiio.h - multi-channel audio I/O configuration (Theme H,
 * issue #132)
 *
 * The graph carries multi-channel buses (issue #119); this is the device side -
 * an interface with more than two input/output channels (a multi-out DAC, a
 * four-in ADC) and the routing between the device's physical channels and the
 * graph's input/output bus channels.
 *
 * Hardware delivers frames *interleaved* (L R L R ...), while the graph and the
 * DSP work on *planar* per-channel buffers, so this module does the
 * de-interleave on capture and the interleave on playback, plus a routing matrix
 * that maps each device channel to a graph channel (or silence) - so you can
 * send a mono source to two outputs, swap a pair, or leave a channel unpatched.
 *
 * int16 PCM, integer only - runs on the -mgeneral-regs-only audio path.  Pure,
 * host-tested (make test-arm-multiio); no allocation, no libc, no FP.
 */

#ifndef ARM64_MULTIIO_H
#define ARM64_MULTIIO_H

#include <stdint.h>

#define IO_MAX_CHANNELS 8

#define IO_SILENCE (-1)   /* a routing slot fed by silence */

typedef struct {
    int n_in;                        /* device input (capture) channels  */
    int n_out;                       /* device output (playback) channels */
    /* out_src[d] = the graph channel feeding device output channel d, or
     * IO_SILENCE. */
    int out_src[IO_MAX_CHANNELS];
    /* in_src[g] = the device input channel feeding graph input channel g, or
     * IO_SILENCE. */
    int in_src[IO_MAX_CHANNELS];
} io_config_t;

/* Initialise for `n_in` capture and `n_out` playback channels (each clamped to
 * IO_MAX_CHANNELS) with identity routing: graph channel i <-> device channel i
 * where both exist, silence otherwise. */
void io_config_init(io_config_t *cfg, int n_in, int n_out);

/* Route device output channel `dev_ch` from graph channel `graph_ch` (or
 * IO_SILENCE).  Returns 0 on success, -1 on a bad index. */
int  io_route_out(io_config_t *cfg, int dev_ch, int graph_ch);
/* Route graph input channel `graph_ch` from device input channel `dev_ch` (or
 * IO_SILENCE).  Returns 0 on success, -1 on a bad index. */
int  io_route_in(io_config_t *cfg, int graph_ch, int dev_ch);

/* Split `n_frames` of `n_ch`-channel interleaved PCM into planar per-channel
 * buffers `planes[0..n_ch)` (each holds `n_frames`). */
void io_deinterleave(const int16_t *in, int n_frames, int n_ch, int16_t *const *planes);

/* Merge planar per-channel buffers into `n_ch`-channel interleaved PCM. */
void io_interleave(int16_t *const *planes, int n_frames, int n_ch, int16_t *out);

/* Capture: de-interleave the device input, then route into the graph's planar
 * input channels.  `dev_in` is n_in-channel interleaved (`n_frames`); `graph_in`
 * has `n_graph_ch` planes.  Unrouted graph channels are filled with silence. */
void io_capture(const io_config_t *cfg, const int16_t *dev_in, int n_frames,
                int16_t *const *graph_in, int n_graph_ch);

/* Playback: route the graph's planar output channels to the device channels,
 * then interleave into `dev_out` (n_out-channel interleaved).  Device channels
 * routed to silence (or to a missing graph channel) output zeros. */
void io_playback(const io_config_t *cfg, int16_t *const *graph_out, int n_graph_ch,
                 int n_frames, int16_t *dev_out);

#endif /* ARM64_MULTIIO_H */
