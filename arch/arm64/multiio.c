/* arch/arm64/multiio.c - multi-channel audio I/O configuration (Theme H,
 * issue #132).  See multiio.h. */

#include "multiio.h"

static int clampch(int n)
{
    if (n < 0) return 0;
    if (n > IO_MAX_CHANNELS) return IO_MAX_CHANNELS;
    return n;
}

void io_config_init(io_config_t *cfg, int n_in, int n_out)
{
    cfg->n_in  = clampch(n_in);
    cfg->n_out = clampch(n_out);
    for (int i = 0; i < IO_MAX_CHANNELS; i++) {
        /* Identity where both sides have the channel, silence otherwise. */
        cfg->out_src[i] = (i < cfg->n_out) ? i : IO_SILENCE;
        cfg->in_src[i]  = (i < cfg->n_in)  ? i : IO_SILENCE;
    }
}

int io_route_out(io_config_t *cfg, int dev_ch, int graph_ch)
{
    if (dev_ch < 0 || dev_ch >= cfg->n_out)
        return -1;
    if (graph_ch != IO_SILENCE && (graph_ch < 0 || graph_ch >= IO_MAX_CHANNELS))
        return -1;
    cfg->out_src[dev_ch] = graph_ch;
    return 0;
}

int io_route_in(io_config_t *cfg, int graph_ch, int dev_ch)
{
    if (graph_ch < 0 || graph_ch >= IO_MAX_CHANNELS)
        return -1;
    if (dev_ch != IO_SILENCE && (dev_ch < 0 || dev_ch >= cfg->n_in))
        return -1;
    cfg->in_src[graph_ch] = dev_ch;
    return 0;
}

void io_deinterleave(const int16_t *in, int n_frames, int n_ch, int16_t *const *planes)
{
    for (int c = 0; c < n_ch; c++)
        for (int f = 0; f < n_frames; f++)
            planes[c][f] = in[f * n_ch + c];
}

void io_interleave(int16_t *const *planes, int n_frames, int n_ch, int16_t *out)
{
    for (int f = 0; f < n_frames; f++)
        for (int c = 0; c < n_ch; c++)
            out[f * n_ch + c] = planes[c][f];
}

void io_capture(const io_config_t *cfg, const int16_t *dev_in, int n_frames,
                int16_t *const *graph_in, int n_graph_ch)
{
    for (int g = 0; g < n_graph_ch; g++) {
        int src = (g < IO_MAX_CHANNELS) ? cfg->in_src[g] : IO_SILENCE;
        if (src >= 0 && src < cfg->n_in) {
            for (int f = 0; f < n_frames; f++)
                graph_in[g][f] = dev_in[f * cfg->n_in + src];
        } else {
            for (int f = 0; f < n_frames; f++)
                graph_in[g][f] = 0;      /* unrouted -> silence */
        }
    }
}

void io_playback(const io_config_t *cfg, int16_t *const *graph_out, int n_graph_ch,
                 int n_frames, int16_t *dev_out)
{
    for (int d = 0; d < cfg->n_out; d++) {
        int src = cfg->out_src[d];
        if (src >= 0 && src < n_graph_ch) {
            for (int f = 0; f < n_frames; f++)
                dev_out[f * cfg->n_out + d] = graph_out[src][f];
        } else {
            for (int f = 0; f < n_frames; f++)
                dev_out[f * cfg->n_out + d] = 0;   /* silence / missing source */
        }
    }
}
