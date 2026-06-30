/* arch/arm64/audio_core.h - dedicated audio-core loop + watchdog (Issue #21)
 *
 * The audio thread runs alone on CPU0.  Its structure is the classic RT audio
 * loop: sleep (WFI) until the DMA-buffer-empty interrupt, wake, refill the DMA
 * buffer from the lock-free ring, then sleep again.  It never yields and never
 * blocks on a lock held by a non-RT task.
 *
 * A watchdog times each callback against its deadline (the time budget between
 * two DMA interrupts) and counts overruns, which are logged over UART; under
 * normal operation the count stays at zero.  The watchdog accounting is pure
 * and host-tested.
 */

#ifndef ARM64_AUDIO_CORE_H
#define ARM64_AUDIO_CORE_H

#include "spsc_ring.h"
#include <stdint.h>

/* ---- pure watchdog (host-testable) ---------------------------------- */

typedef struct {
    uint64_t budget;     /* cycles allowed per callback (the deadline)   */
    uint64_t count;      /* callbacks accounted                          */
    uint64_t overruns;   /* callbacks that missed the deadline           */
    uint64_t worst;      /* worst-case service time seen (cycles)        */
} audio_wd_t;

void audio_wd_init(audio_wd_t *wd, uint64_t budget_cycles);

/* Account one callback that took `service_cycles`.  Returns 1 if it overran
 * its deadline (and bumps the overrun counter), 0 otherwise. */
int audio_wd_account(audio_wd_t *wd, uint64_t service_cycles);

/* ---- audio-core runtime --------------------------------------------- */

typedef struct {
    spsc_ring_t  *ring;       /* samples produced by the host (CPU1-3)    */
    int16_t      *dma_buf;    /* the buffer the DMA drains                */
    uint32_t      frames;     /* frames refilled per callback             */
    audio_wd_t    wd;
    volatile uint64_t serviced;  /* callbacks completed                   */
    volatile uint64_t max_wake;  /* worst IRQ->wake latency seen (cycles) */
} audio_core_t;

void audio_core_init(audio_core_t *ac, spsc_ring_t *ring, int16_t *dma_buf,
                     uint32_t frames, uint64_t budget_cycles);

/* Move one callback period of samples from the ring into the DMA buffer:
 * pulls 2*frames samples (stereo), zero-filling any shortfall (silence rather
 * than stale audio on underrun).  Returns the number of samples actually read
 * from the ring (less than 2*frames signals an underrun).  Does not touch the
 * watchdog, so the caller can time the move and account it itself. */
uint32_t audio_core_fill(audio_core_t *ac);

/* audio_core_fill() followed by audio_wd_account(service_cycles) and a bump of
 * the serviced counter; convenient when the caller already knows the service
 * time (e.g. the host tests). */
void audio_core_refill(audio_core_t *ac, uint64_t service_cycles);

#endif /* ARM64_AUDIO_CORE_H */
