#include "session_shell.h"
#include "shell_graph.h"
#include <stddef.h>
static int eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static const char *prefix(const char *a, const char *b) {
  while (*b) {
    if (*a++ != *b++)
      return NULL;
  }
  return a;
}
static void number(shell_t *sh, uint64_t v) {
  char rev[20], out[21];
  unsigned n = 0, k = 0;
  do {
    rev[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    out[k++] = rev[--n];
  out[k] = 0;
  shell_write(sh, out);
}
static audio_session_t *session(shell_t *sh) { return sh->ctx; }
static session_scene_t *scene(shell_t *sh) {
  audio_session_t *s = session(sh);
  return &s->scene[__atomic_load_n(&s->active, __ATOMIC_ACQUIRE)];
}
static int index_of(shell_t *sh, uint32_t pid) {
  session_scene_t *sc = scene(sh);
  for (unsigned i = 0; i < sc->count; ++i)
    if (sc->nodes[i]->pid == pid)
      return (int)i;
  return -1;
}
static int pid(const char *text, uint32_t *value) {
  if (eq(text, "dac")) {
    *value = 0;
    return 0;
  }
  return sg_parse_u32(text, value);
}
int session_parse_duration(const char *text, uint64_t hz, uint64_t frame,
                           uint64_t *out) {
  if (!text || !out || !hz || !frame)
    return TC_EINVAL;
  char buf[32];
  unsigned n = 0;
  while (text[n] >= '0' && text[n] <= '9') {
    if (n >= sizeof(buf) - 1)
      return TC_EINVAL;
    buf[n] = text[n];
    ++n;
  }
  buf[n] = 0;
  uint64_t v;
  if (!n || session_parse_u64(buf, &v) || !v)
    return TC_EINVAL;
  uint64_t scale = 1, den = 1;
  if (eq(text + n, "us")) {
    scale = hz;
    den = 1000000;
  } else if (eq(text + n, "ms")) {
    scale = hz;
    den = 1000;
  } else if (eq(text + n, "blocks")) {
    scale = frame;
  } else if (!eq(text + n, "ticks"))
    return TC_EINVAL;
  if (v > UINT64_MAX / scale)
    return TC_EINVAL;
  uint64_t product = v * scale;
  /* Round up, so a requested allowance is never silently rounded to zero. */
  *out = product / den + (product % den != 0);
  return 0;
}
static int report(shell_t *sh, int rc) {
  if (rc == TC_OK) {
    shell_write(sh, "ok\n");
    return 0;
  }
  audio_session_t *s = session(sh);
  if (rc == TC_EADMISSION) {
    shell_write(sh, "not admitted: pid=");
    number(sh, s->admission.pid);
    shell_write(sh, " required_ticks=");
    number(sh, s->admission.required);
    shell_write(sh, " available_ticks=");
    number(sh, s->admission.available);
    shell_write(sh, "\n");
  } else if (rc == TC_EDEPENDENCY)
    shell_write(sh, "incompatible dependency, rate, or criticality\n");
  else if (rc == TC_ENODEV)
    shell_write(sh, "unknown plugin or empty graph\n");
  else if (rc == SESSION_EPLUGIN)
    shell_write(sh, "plugin load/callback rejected\n");
  else if (rc == SESSION_EFORMAT)
    shell_write(sh, "invalid session file; previous graph retained\n");
  else if (rc == SESSION_EINPROGRESS)
    shell_write(sh, "boundary swap already accepted; completion pending\n");
  else if (rc == TC_EBUSY)
    shell_write(sh, "control transaction busy\n");
  return rc;
}
static int load(shell_t *sh, int argc, char **v) {
  if (argc != 2)
    return TC_EINVAL;
  audio_session_t *s = session(sh);
  temporal_contract_t c = {.period = s->limits.time.frame_ticks,
                           .deadline = s->limits.time.frame_ticks,
                           .cpu_budget = s->limits.time.frame_ticks / 8,
                           .criticality = TC_SOFT,
                           .overrun_policy = TC_MUTE_THEN_KILL,
                           .kill_after = 3};
  long r = session_load(s, v[1], &c);
  if (r <= 0)
    return report(sh, (int)r);
  shell_write(sh, "loaded pid ");
  number(sh, (uint64_t)r);
  shell_write(sh, "\n");
  return 0;
}
static int unload(shell_t *sh, int argc, char **v) {
  uint32_t p;
  if (argc != 2 || pid(v[1], &p) || !p)
    return TC_EINVAL;
  return report(sh, session_unload(session(sh), p));
}
static int wire(shell_t *sh, int argc, char **v) {
  uint32_t a, b;
  if ((argc != 3 && argc != 4) || pid(v[1], &a) || pid(v[2], &b) || !a ||
      (argc == 4 && !eq(v[3], "feedback")))
    return TC_EINVAL;
  return report(sh, session_wire(session(sh), a, b, argc == 4));
}
static int unwire(shell_t *sh, int argc, char **v) {
  uint32_t a, b;
  if (argc != 3 || pid(v[1], &a) || pid(v[2], &b) || !a)
    return TC_EINVAL;
  return report(sh, session_unwire(session(sh), a, b));
}
static int parameter(shell_t *sh, int argc, char **v) {
  uint32_t p, id, bits;
  if (argc != 4 || pid(v[1], &p) || !p || sg_parse_u32(v[2], &id) ||
      patch_parse_value(v[3], &bits))
    return TC_EINVAL;
  return report(sh, session_set_param(session(sh), p, id, bits));
}
static int contract(shell_t *sh, int argc, char **v) {
  uint32_t p;
  if (argc < 3 || pid(v[1], &p) || !p)
    return TC_EINVAL;
  int i = index_of(sh, p);
  if (i < 0)
    return TC_ENODEV;
  temporal_contract_t c = scene(sh)->contracts[i];
  audio_session_t *s = session(sh);
  if (eq(v[2], "hard"))
    c.criticality = TC_HARD;
  else if (eq(v[2], "soft"))
    c.criticality = TC_SOFT;
  else if (eq(v[2], "best"))
    c.criticality = TC_BEST_EFFORT;
  else
    return TC_EINVAL;
  uint32_t seen = 0, arg = 0;
  int argument = 0;
  for (int k = 3; k < argc; ++k) {
    const char *x;
    unsigned bit;
    uint64_t ticks;
    if ((x = prefix(v[k], "budget=")))
      bit = 0;
    else if ((x = prefix(v[k], "deadline=")))
      bit = 1;
    else if ((x = prefix(v[k], "period=")))
      bit = 2;
    else if ((x = prefix(v[k], "policy=")))
      bit = 3;
    else if ((x = prefix(v[k], "kill=")))
      bit = 4;
    else if ((x = prefix(v[k], "skip=")))
      bit = 5;
    else if (k == 3) {
      x = v[k];
      bit = 0;
    } else
      return TC_EINVAL;
    if (seen & (1u << bit))
      return TC_EINVAL;
    seen |= 1u << bit;
    if (bit < 3) {
      if (session_parse_duration(x, s->counter_hz, s->limits.time.frame_ticks,
                                 &ticks))
        return TC_EINVAL;
      if (bit == 0)
        c.cpu_budget = ticks;
      else if (bit == 1)
        c.deadline = ticks;
      else
        c.period = ticks;
    } else if (bit == 3) {
      if (eq(x, "mute"))
        c.overrun_policy = TC_MUTE;
      else if (eq(x, "bypass"))
        c.overrun_policy = TC_BYPASS;
      else if (eq(x, "kill"))
        c.overrun_policy = TC_KILL;
      else if (eq(x, "strikes"))
        c.overrun_policy = TC_MUTE_THEN_KILL;
      else if (eq(x, "degrade"))
        c.overrun_policy = TC_DEGRADE;
      else
        return TC_EINVAL;
    } else {
      if (argument || sg_parse_u32(x, &arg) || !arg)
        return TC_EINVAL;
      argument = (int)bit;
    }
  }
  if (argument == 4 && c.overrun_policy != TC_MUTE_THEN_KILL)
    return TC_EINVAL;
  if (argument == 5 && c.overrun_policy != TC_DEGRADE)
    return TC_EINVAL;
  c.kill_after = c.overrun_policy == TC_MUTE_THEN_KILL
                     ? (argument       ? arg
                        : c.kill_after ? c.kill_after
                                       : 3)
                     : 0;
  c.skip_periods = c.overrun_policy == TC_DEGRADE
                       ? (argument         ? arg
                          : c.skip_periods ? c.skip_periods
                                           : 1)
                       : 0;
  return report(sh, session_set_contract(s, &c));
}
static int cores(shell_t *sh, int argc, char **v) {
  uint32_t n;
  if (argc != 2 || sg_parse_u32(v[1], &n))
    return TC_EINVAL;
  return report(sh, session_set_cores(session(sh), n));
}
static int pin(shell_t *sh, int argc, char **v) {
  uint32_t p, n;
  if (argc != 3 || pid(v[1], &p) || !p)
    return TC_EINVAL;
  if (eq(v[2], "auto"))
    n = 0;
  else if (sg_parse_u32(v[2], &n) || !n)
    return TC_EINVAL;
  return report(sh, session_pin(session(sh), p, n));
}
static const char *classes[] = {"hard", "soft", "best"};
static const char *policies[] = {"mute", "bypass", "kill", "strikes",
                                 "degrade"};
static int list(shell_t *sh, int argc, char **v) {
  (void)v;
  if (argc != 1)
    return TC_EINVAL;
  session_scene_t *sc = scene(sh);
  shell_write(sh, "graph: cores=");
  number(sh, sc->cores);
  shell_write(sh, session(sh)->running ? " running\n" : " paused\n");
  for (unsigned i = 0; i < sc->count; ++i) {
    const temporal_contract_t *c = &sc->contracts[i];
    shell_write(sh, "pid=");
    number(sh, c->pid);
    shell_write(sh, " ");
    for (unsigned j = 0; j < PM_MAX_PLUGINS; ++j)
      if (sc->nodes[i]->manager->slots[j].used &&
          sc->nodes[i]->manager->slots[j].pid == c->pid)
        shell_write(sh, sc->nodes[i]->manager->slots[j].path);
    shell_write(sh, " cpu=");
    number(sh, sc->plan.core[i]);
    shell_write(sh, " ");
    shell_write(sh, classes[c->criticality]);
    shell_write(sh, " budget=");
    number(sh, c->cpu_budget);
    shell_write(sh, "ticks deadline=");
    number(sh, c->deadline);
    shell_write(sh, "ticks period=");
    number(sh, c->period);
    shell_write(sh, "ticks policy=");
    shell_write(sh, policies[c->overrun_policy]);
    shell_write(sh, "\n");
  }
  for (unsigned e = 0; e < GRAPH_MAX_EDGES; ++e) {
    graph_edge_t *edge = &sc->graph.graph.edges[e];
    if (!edge->used)
      continue;
    number(sh, sc->graph.graph.nodes[edge->src].pid);
    shell_write(sh, " -> ");
    if (edge->dst == sc->graph.graph.dac_node)
      shell_write(sh, "dac");
    else
      number(sh, sc->graph.graph.nodes[edge->dst].pid);
    shell_write(sh, edge->feedback ? " feedback\n" : "\n");
  }
  return 0;
}
static void field(shell_t *sh, const char *name, uint64_t n) {
  shell_write(sh, name);
  number(sh, n);
}
static int stats(shell_t *sh, int argc, char **v) {
  (void)v;
  if (argc != 1)
    return TC_EINVAL;
  audio_session_t *s = session(sh);
  session_scene_t *sc = scene(sh);
  shell_write(sh, "audio:");
  field(sh,
        " frames=", __atomic_load_n(&s->runtime.accepted, __ATOMIC_RELAXED));
  field(sh,
        " skipped=", __atomic_load_n(&s->runtime.skipped, __ATOMIC_RELAXED));
  field(sh, " pause_silence=",
        __atomic_load_n(&s->intentional_silence, __ATOMIC_RELAXED));
  shell_write(sh, "\n");
  for (unsigned i = 0; i < sc->count; ++i) {
    tc_task_state_t t;
    int rc = tm_snapshot(&s->runtime, sc->nodes[i]->pid, &t);
    shell_write(sh, "plugin");
    field(sh, " pid=", sc->nodes[i]->pid);
    if (rc != TC_OK) {
      shell_write(sh, " snapshot busy\n");
      continue;
    }
    field(sh, " cpu=", sc->plan.core[i]);
    field(sh, " runs=", t.runs);
    field(sh, " completed=", t.completed);
    field(sh, " budget=", t.budget_overruns);
    field(sh, " deadline=", t.deadline_misses);
    field(sh, " shed=", t.shed);
    field(sh, " killed=", t.killed);
    field(sh, " max_ticks=", t.service_max);
    shell_write(sh, "\n");
  }
  return 0;
}
static void filename(void *ctx, const char *name) {
  shell_t *sh = ctx;
  shell_write(sh, "  ");
  shell_write(sh, name);
  shell_write(sh, "\n");
}
static int patch(shell_t *sh, int argc, char **v) {
  if (argc < 2)
    return TC_EINVAL;
  if (eq(v[1], "ls")) {
    if (argc != 2)
      return TC_EINVAL;
    vfs_t *fs = &session(sh)->manager[0].vfs;
    shell_write(sh, "files:\r\n");
    for (unsigned i = 0; i < VFS_STORE_SLOTS; ++i)
      if (fs->store[i].used)
        filename(sh, fs->store[i].name);
    return fs->fat
               ? report(sh,
                        fat_list(fs->fat, filename, sh) < 0 ? SESSION_EIO : 0)
               : 0;
  }
  if (argc != 3)
    return TC_EINVAL;
  if (eq(v[1], "save"))
    return report(sh, session_save(session(sh), v[2]));
  if (eq(v[1], "load"))
    return report(sh, session_restore(session(sh), v[2]));
  return TC_EINVAL;
}
static int start(shell_t *sh, int argc, char **v) {
  (void)v;
  return argc == 1 ? report(sh, session_start(session(sh))) : TC_EINVAL;
}
static int pause_cmd(shell_t *sh, int argc, char **v) {
  (void)v;
  return argc == 1 ? report(sh, session_pause(session(sh))) : TC_EINVAL;
}
static int clear(shell_t *sh, int argc, char **v) {
  (void)v;
  return argc == 1 ? report(sh, session_clear(session(sh))) : TC_EINVAL;
}
static const shell_cmd_t commands[] = {
    {"load", "load <path> (soft, 1/8 frame default budget)", load},
    {"unload", "unload <pid>", unload},
    {"wire", "wire <src> <dst|dac> [feedback]", wire},
    {"connect", "alias of wire", wire},
    {"unwire", "unwire <src> <dst|dac>", unwire},
    {"set-param", "set-param <pid> <id> <decimal|0xBITS>", parameter},
    {"contract",
     "contract <pid> hard|soft|best [budget] [deadline=.. period=.. policy=.. "
     "kill=N|skip=N]",
     contract},
    {"cores", "cores <1..3>", cores},
    {"pin", "pin <pid> <1..3|auto>", pin},
    {"ls", "graph, placement and contracts (ticks)", list},
    {"contracts", "alias of ls", list},
    {"stats", "live per-plugin counters", stats},
    {"patch", "patch save|load <path>; patch ls", patch},
    {"start", "admit and start", start},
    {"run", "alias of start", start},
    {"pause", "pause and drain", pause_cmd},
    {"clear", "unload all plugins; files remain", clear}};
void session_shell_init(shell_t *sh, audio_session_t *s,
                        void (*out)(void *, const char *), void *ctx) {
  shell_init(sh, commands, (int)(sizeof(commands) / sizeof(commands[0])), out,
             ctx);
  sh->ctx = s;
  sh->prompt = "tessera> ";
}
