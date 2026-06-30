/* arch/arm64/plugin_host.h - resilient audio host (Issue #26, M5)
 *
 * The host is the piece that owns the DMA/I2S output and, on every audio block,
 * drains a plugin's shared ring buffer (issue #25) into the output.  It must
 * survive the plugin dying: if the ring runs dry or the kernel has marked the
 * producer DEAD, the host substitutes silence, counts the overrun, logs once,
 * and keeps running - audio output never stops and the host never crashes.
 *
 * This is the ARM port of the IKOS audio-host loop (kernel/audio.c); the
 * ring-draining/death-detection policy is pure, so it is unit-tested on the
 * host (make test-arm-plugin-host).
 */

#ifndef ARM64_PLUGIN_HOST_H
#define ARM64_PLUGIN_HOST_H

#include "audio_ringbuf.h"
#include <stdint.h>

typedef struct {
    audio_ring_hdr_t *ring;     /* the plugin's output ring                 */
    uint32_t frames;            /* frames per audio block                   */
    uint64_t overruns;          /* blocks that needed silence substituted   */
    uint32_t dead_logged;       /* the producer-death log fired once         */
} plugin_host_t;

void host_init(plugin_host_t *h, audio_ring_hdr_t *ring, uint32_t frames);

/* True once the kernel has marked the ring's producer DEAD. */
int host_producer_dead(const plugin_host_t *h);

/* Produce one audio block of `frames` interleaved stereo samples into `out`
 * (2*frames floats).  Returns 1 if the block was filled entirely from the
 * plugin (real audio), 0 if any silence had to be substituted (an overrun).
 * On the first detected producer death it calls the weak host_on_death() hook
 * (the harness logs it over UART). */
int host_block(plugin_host_t *h, float *out);

/* Weak hook invoked once when producer death is first detected.  Default is a
 * no-op; the harness/system overrides it to log over UART and apply a restart
 * policy. */
void host_on_death(plugin_host_t *h);

#endif /* ARM64_PLUGIN_HOST_H */
