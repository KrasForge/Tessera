/* arch/arm64/tempo_sync.h - tempo-synced note values and tap tempo
 *                          (Theme C, issue #115)
 *
 * Once there is a master transport (#114), musical parameters - delay time, LFO
 * rate - should lock to the tempo (1/4, dotted 1/8, triplets) rather than
 * free-run, and the player needs a tap-tempo control to set the tempo by feel.
 *
 * A note value is a fraction of a quarter note carried as (mult, div): a quarter
 * is (1,1), an eighth (1,2), a dotted eighth (3,4), a quarter triplet (2,3).
 * The delay/period math is exact integer arithmetic; no floating point.
 * Unit-tested on the host (make test-arm-tempo-sync).
 */

#ifndef ARM64_TEMPO_SYNC_H
#define ARM64_TEMPO_SYNC_H

#include <stdint.h>

/* Common note values as (mult, div) quarter-note fractions - pass as two args. */
#define TS_WHOLE         4u, 1u
#define TS_HALF          2u, 1u
#define TS_QUARTER       1u, 1u
#define TS_EIGHTH        1u, 2u
#define TS_SIXTEENTH     1u, 4u
#define TS_DOT_QUARTER   3u, 2u
#define TS_DOT_EIGHTH    3u, 4u
#define TS_TRIP_QUARTER  2u, 3u
#define TS_TRIP_EIGHTH   1u, 3u

/* Length of a note value in samples at (tempo_mbpm, sr):
 *   quarter_samples = sr * 60000 / tempo_mbpm ; result = quarter * mult / div. */
uint32_t tempo_sync_samples(uint32_t tempo_mbpm, uint32_t sr, uint32_t mult, uint32_t div);

/* Length of a note value in milliseconds at tempo_mbpm. */
uint32_t tempo_sync_ms(uint32_t tempo_mbpm, uint32_t mult, uint32_t div);

/* ---- tap tempo ----------------------------------------------------------- *
 * Averages the last few inter-tap intervals and rejects an outlier (a tap more
 * than 2x away from the running interval), so a mis-tap does not lurch the
 * tempo.  A tap far outside that window restarts the estimate (a new tempo). */
#define TS_TAPS 4u
typedef struct {
    uint32_t interval[TS_TAPS];   /* recent accepted inter-tap intervals (frames) */
    uint32_t n;                   /* valid entries (<= TS_TAPS)                    */
    uint32_t head;                /* next write slot                              */
    uint32_t avg;                 /* running average interval (frames)            */
    uint32_t pending;             /* a rejected outlier, held to detect a new tempo */
} taptempo_t;

void taptempo_init(taptempo_t *tt);

/* Register a tap `frames_since_last` frames after the previous one.  Returns 1
 * if it was folded into the estimate, 0 if rejected as a single outlier. */
int  taptempo_tap(taptempo_t *tt, uint32_t frames_since_last);

/* Estimated tempo in milli-BPM from the accepted taps, or 0 with none. */
uint32_t taptempo_mbpm(const taptempo_t *tt, uint32_t sr);

#endif /* ARM64_TEMPO_SYNC_H */
