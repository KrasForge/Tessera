/* Versioned M13 session persistence, including temporal contracts/affinity. */
#ifndef ARM64_SESSION_PRESET_H
#define ARM64_SESSION_PRESET_H
#include "patch.h"
#include "temporal_multicore.h"
#define SESSION_TEXT_MAX 4096u
#define SESSION_EFORMAT (-32)
typedef struct {
  patch_t patch;
  temporal_contract_t contract[PATCH_MAX_PLUGINS];
  uint8_t pin[PATCH_MAX_PLUGINS], feedback[PATCH_MAX_EDGES];
  uint32_t sample_rate, frames, cores;
  uint64_t counter_hz;
} session_preset_t;
int session_parse_u64(const char *text, uint64_t *out);
/* Transactional parser; exact integer ticks/float bit patterns. A different
 * sample clock/timebase must be explicitly converted, never silently applied.
 */
int session_preset_parse(const char *text, uint32_t len, session_preset_t *out);
long session_preset_write(const session_preset_t *p, char *text,
                          uint32_t capacity);
int session_preset_plan(const session_preset_t *p, const tm_limits_t *limits,
                        tm_plan_t *out, tc_admission_t *why);
#endif
