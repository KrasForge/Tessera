/* M11/M13 shared application. Stable isolated instances, immutable graph
 * configurations, frame-boundary replacement, and control-side reclamation. */
#ifndef ARM64_AUDIO_SESSION_H
#define ARM64_AUDIO_SESSION_H
#include "plugin_mgr.h"
#include "session_preset.h"
#define SESSION_MAX_FRAMES 256u
#define SESSION_EPLUGIN (-33)
#define SESSION_EIO (-34)
#define SESSION_ETIMEOUT (-35)
#define SESSION_EFULL (-36)
#define SESSION_EINPROGRESS (-37)
#define SESSION_INSTANCES (PM_MAX_PLUGINS * 2u)
#define SESSION_IN_VA (USER_VA_BASE + 0x0b000000ull)
#define SESSION_OUT_VA (USER_VA_BASE + 0x0c000000ull)
typedef struct {
  param_queue_t queue;
  param_event_t events[PM_PARAM_CAP]; /* trusted, never plugin-writable */
} session_params_t;
typedef struct {
  uint32_t pid, used, frames;
  plugin_t *plugin;
  plugin_mgr_t *manager;
  uint32_t *input, *scratch;
  uint32_t output[2][SESSION_MAX_FRAMES * 2];
  session_params_t parameters;
} session_node_t;
struct audio_session;
typedef struct {
  struct audio_session *owner;
  graph_control_t graph;
  session_node_t *nodes[TC_MAX_TASKS];
  temporal_contract_t contracts[TC_MAX_TASKS];
  uint8_t pins[TC_MAX_TASKS];
  uint32_t count, cores, rings[GRAPH_MAX_EDGES];
  tm_plan_t plan;
} session_scene_t;
typedef struct audio_session {
  session_scene_t scene[2]; /* unchanged while any worker references it */
  plugin_mgr_t manager[2];  /* stable slots, full old+new graph can coexist */
  graph_control_t lifecycle_graph[2];
  session_node_t instances[SESSION_INSTANCES];
  tm_runtime_t runtime;
  tm_limits_t limits;
  uint64_t counter_hz, lifecycle_ticks, timeout_ticks;
  uint64_t (*clock)(void);
  uint32_t sample_rate, frames, available_mask;
  uint32_t active, editing, readers, running;
  uint32_t pending, retire; /* bank+1 pending; bit2 = claimed by cadence */
  int swap_result, last_kick_result;
  uint64_t invalid_kicks;
  uint64_t collected_frame, intentional_silence, unavailable, swaps;
  tc_admission_t admission;
  long last_lifecycle_result;
  char text[SESSION_TEXT_MAX];
} audio_session_t;
/* available_mask is 2 (CPU1), 6 (CPU1/2), or 14 (CPU1/2/3). Reserve a
 * separate control CPU whenever control can wait for a boundary commit. */
int session_init(audio_session_t *s, audio_worker_t *workers[TM_CORES],
                 const tm_limits_t *limits, uint64_t (*clock)(void),
                 uint64_t hz, uint32_t sample_rate, uint32_t frames,
                 uint64_t lifecycle_ticks, uint32_t available_mask);
/* Configure source registry/storage while stopped, before control runs. */
int session_add_blob(audio_session_t *s, const char *name, const void *bytes,
                     uint32_t len);
void session_mount(audio_session_t *s, fat_fs_t *fat);
/* Edits stage and admit a replacement without stopping existing DSP. CPU0
 * swaps it only after collecting the previous complete frame. Removed
 * instances are destroyed on the control CPU, after boundary acknowledgement.
 * A failed admission/load leaves the active graph unchanged. A timeout before
 * claim cancels; EINPROGRESS means a claimed swap is awaiting acknowledgement.
 */
long session_load(audio_session_t *s, const char *path,
                  const temporal_contract_t *contract);
int session_unload(audio_session_t *s, uint32_t pid);
int session_wire(audio_session_t *s, uint32_t src, uint32_t dst, int feedback);
int session_unwire(audio_session_t *s, uint32_t src, uint32_t dst);
int session_set_contract(audio_session_t *s,
                         const temporal_contract_t *contract);
int session_set_cores(audio_session_t *s, unsigned cores);
int session_pin(audio_session_t *s, uint32_t pid, unsigned core);
int session_set_param(audio_session_t *s, uint32_t pid, uint32_t id,
                      uint32_t bits);
int session_start(audio_session_t *s);
int session_pause(audio_session_t *s);
int session_clear(audio_session_t *s);
/* One cadence producer: collect, commit, kick. Never wait/allocate/free or
 * invoke lifecycle callbacks. out contains interleaved stereo PCM16.
 * Cold start and explicit pause supply silence; graph edits do not pause. */
int session_tick(audio_session_t *s, uint64_t frame, uint64_t release,
                 int16_t *out);
int session_collect(audio_session_t *s, int16_t *out);
int session_capture(audio_session_t *s, session_preset_t *out);
int session_save(audio_session_t *s, const char *path);
int session_restore(audio_session_t *s, const char *path);
#endif
