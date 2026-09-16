/* M13 serial frontend to the same M11 admitted execution engine. */
#ifndef ARM64_SESSION_SHELL_H
#define ARM64_SESSION_SHELL_H
#include "audio_session.h"
#include "shell.h"
/* Single control-plane caller, off the audio IRQ. Commands can pause workers
 * for safe reconfiguration, but never block the cadence core's interrupt. */
void session_shell_init(shell_t *sh, audio_session_t *session,
                        void (*out)(void *, const char *), void *ctx);
int session_parse_duration(const char *s, uint64_t hz, uint64_t frame_ticks,
                           uint64_t *ticks);
#endif
