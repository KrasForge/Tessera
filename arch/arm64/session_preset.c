#include "session_preset.h"
#include <stddef.h>
static int eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
int session_parse_u64(const char *s, uint64_t *out) {
  if (!s || !out || !*s)
    return SESSION_EFORMAT;
  unsigned base = 10;
  uint64_t v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
    if (!*s)
      return SESSION_EFORMAT;
  }
  for (; *s; ++s) {
    unsigned d = 255;
    if (*s >= '0' && *s <= '9')
      d = (unsigned)(*s - '0');
    else if (*s >= 'a' && *s <= 'f')
      d = 10u + (unsigned)(*s - 'a');
    else if (*s >= 'A' && *s <= 'F')
      d = 10u + (unsigned)(*s - 'A');
    if (d >= base || v > (UINT64_MAX - d) / base)
      return SESSION_EFORMAT;
    v = v * base + d;
  }
  *out = v;
  return 0;
}
static int tokens(char *line, char **v, unsigned max) {
  unsigned n = 0;
  while (*line) {
    while (*line == ' ' || *line == '\t' || *line == '\r')
      *line++ = 0;
    if (!*line)
      break;
    if (n == max)
      return -1;
    v[n++] = line;
    while (*line && *line != ' ' && *line != '\t' && *line != '\r')
      ++line;
  }
  return (int)n;
}
int session_preset_parse(const char *text, uint32_t len,
                         session_preset_t *out) {
  if (!text || !out || !len || len > SESSION_TEXT_MAX || text[len - 1] != '\n')
    return SESSION_EFORMAT;
  session_preset_t p = {0};
  unsigned position = 0, header = 0, audio = 0;
  while (position < len) {
    char line[256];
    unsigned n = 0;
    while (position < len && text[position] != '\n') {
      unsigned char ch = (unsigned char)text[position++];
      if ((ch < 32 && ch != '\t' && ch != '\r') || ch > 126 ||
          n >= sizeof(line) - 1)
        return SESSION_EFORMAT;
      line[n++] = (char)ch;
    }
    if (position >= len)
      return SESSION_EFORMAT;
    ++position;
    line[n] = 0;
    if (!header) {
      if (!eq(line, "# tessera-session v1"))
        return SESSION_EFORMAT;
      header = 1;
      continue;
    }
    char *v[10];
    int argc = tokens(line, v, 10);
    if (argc < 0)
      return SESSION_EFORMAT;
    if (!argc || v[0][0] == '#')
      continue;
    uint64_t x[8] = {0};
    if (eq(v[0], "audio")) {
      if (argc != 5 || audio || p.patch.n_plugins)
        return SESSION_EFORMAT;
      for (unsigned i = 1; i < 5; ++i)
        if (session_parse_u64(v[i], &x[i - 1]))
          return SESSION_EFORMAT;
      if (!x[0] || x[0] > UINT32_MAX || !x[1] || x[1] > 256 || !x[2] ||
          x[2] > UINT32_MAX || x[3] < 1 || x[3] > 3)
        return SESSION_EFORMAT;
      p.sample_rate = (uint32_t)x[0];
      p.frames = (uint32_t)x[1];
      p.counter_hz = x[2];
      p.cores = (uint32_t)x[3];
      audio = 1;
    } else if (eq(v[0], "task")) {
      if (!audio || argc != 9)
        return SESSION_EFORMAT;
      for (unsigned i = 2; i < 9; ++i)
        if (session_parse_u64(v[i], &x[i - 2]))
          return SESSION_EFORMAT;
      if (x[3] > TC_BEST_EFFORT || x[4] > TC_DEGRADE || x[5] > UINT32_MAX ||
          x[6] > 3)
        return SESSION_EFORMAT;
      int i = patch_add_plugin(&p.patch, v[1]);
      if (i < 0)
        return SESSION_EFORMAT;
      p.contract[i] = (temporal_contract_t){.pid = (uint32_t)i + 1,
                                            .period = x[0],
                                            .deadline = x[1],
                                            .cpu_budget = x[2],
                                            .criticality = (uint32_t)x[3],
                                            .overrun_policy = (uint32_t)x[4]};
      if (x[4] == TC_DEGRADE)
        p.contract[i].skip_periods = (uint32_t)x[5];
      else
        p.contract[i].kill_after = (uint32_t)x[5];
      p.pin[i] = (uint8_t)x[6];
    } else if (eq(v[0], "param")) {
      if (argc != 4 || session_parse_u64(v[1], &x[0]) ||
          session_parse_u64(v[2], &x[1]) || session_parse_u64(v[3], &x[2]) ||
          x[0] >= PATCH_MAX_PLUGINS || x[1] > UINT32_MAX || x[2] > UINT32_MAX)
        return SESSION_EFORMAT;
      /* Persistent parameter records are exact bits, not decimal floats. */
      if (patch_add_param(&p.patch, (int)x[0], (uint32_t)x[1], (uint32_t)x[2]) <
          0)
        return SESSION_EFORMAT;
      for (int j = 0; j < p.patch.n_params - 1; ++j)
        if (p.patch.params[j].plugin == (int)x[0] &&
            p.patch.params[j].id == (uint32_t)x[1])
          return SESSION_EFORMAT;
    } else if (eq(v[0], "wire")) {
      if (argc != 4 || session_parse_u64(v[1], &x[0]) ||
          session_parse_u64(v[3], &x[2]) || x[0] >= PATCH_MAX_PLUGINS ||
          x[2] > 1)
        return SESSION_EFORMAT;
      int dst = PATCH_DAC;
      if (!eq(v[2], "dac")) {
        if (session_parse_u64(v[2], &x[1]) || x[1] >= PATCH_MAX_PLUGINS)
          return SESSION_EFORMAT;
        dst = (int)x[1];
      }
      int e = patch_add_edge(&p.patch, (int)x[0], dst);
      if (e < 0 || (dst == PATCH_DAC && x[2]))
        return SESSION_EFORMAT;
      p.feedback[e] = (uint8_t)x[2];
    } else
      return SESSION_EFORMAT;
  }
  if (!header || !audio)
    return SESSION_EFORMAT;
  *out = p;
  return 0;
}
static int put(char *out, uint32_t *pos, uint32_t cap, const char *s) {
  while (*s) {
    if (*pos + 1 >= cap)
      return -1;
    out[(*pos)++] = *s++;
  }
  if (*pos >= cap)
    return -1;
  out[*pos] = 0;
  return 0;
}
static int number(char *out, uint32_t *pos, uint32_t cap, uint64_t n) {
  char b[21], rev[21];
  unsigned k = 0, j = 0;
  do {
    rev[k++] = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  while (k)
    b[j++] = rev[--k];
  b[j] = 0;
  return put(out, pos, cap, b);
}
long session_preset_write(const session_preset_t *p, char *out, uint32_t cap) {
  if (!p || !out || p->patch.n_plugins < 0 ||
      p->patch.n_plugins > PATCH_MAX_PLUGINS || p->patch.n_params < 0 ||
      p->patch.n_params > PATCH_MAX_PARAMS || p->patch.n_edges < 0 ||
      p->patch.n_edges > PATCH_MAX_EDGES)
    return SESSION_EFORMAT;
  uint32_t pos = 0;
#define S(s)                                                                   \
  do {                                                                         \
    if (put(out, &pos, cap, s))                                                \
      return PATCH_ENOSPACE;                                                   \
  } while (0)
#define N(n)                                                                   \
  do {                                                                         \
    if (number(out, &pos, cap, n))                                             \
      return PATCH_ENOSPACE;                                                   \
  } while (0)
  S("# tessera-session v1\naudio ");
  N(p->sample_rate);
  S(" ");
  N(p->frames);
  S(" ");
  N(p->counter_hz);
  S(" ");
  N(p->cores);
  S("\n");
  for (int i = 0; i < p->patch.n_plugins; ++i) {
    const temporal_contract_t *t = &p->contract[i];
    unsigned k = 0;
    while (k < PATCH_PATH_MAX && p->patch.plugins[i].path[k]) {
      unsigned char c = p->patch.plugins[i].path[k++];
      if (c <= 32 || c > 126)
        return SESSION_EFORMAT;
    }
    if (!k || k == PATCH_PATH_MAX)
      return SESSION_EFORMAT;
    S("task ");
    S(p->patch.plugins[i].path);
    S(" ");
    N(t->period);
    S(" ");
    N(t->deadline);
    S(" ");
    N(t->cpu_budget);
    S(" ");
    N(t->criticality);
    S(" ");
    N(t->overrun_policy);
    S(" ");
    N(t->overrun_policy == TC_DEGRADE ? t->skip_periods : t->kill_after);
    S(" ");
    N(p->pin[i]);
    S("\n");
  }
  for (int i = 0; i < p->patch.n_params; ++i) {
    const patch_param_t *v = &p->patch.params[i];
    char hex[11];
    patch_format_value(v->bits, hex);
    S("param ");
    N((unsigned)v->plugin);
    S(" ");
    N(v->id);
    S(" ");
    S(hex);
    S("\n");
  }
  for (int i = 0; i < p->patch.n_edges; ++i) {
    S("wire ");
    N((unsigned)p->patch.edges[i].src);
    S(" ");
    if (p->patch.edges[i].dst == PATCH_DAC) {
      S("dac");
    } else {
      N((unsigned)p->patch.edges[i].dst);
    }
    S(" ");
    N(p->feedback[i]);
    S("\n");
  }
#undef N
#undef S
  return pos;
}
int session_preset_plan(const session_preset_t *p, const tm_limits_t *limits,
                        tm_plan_t *out, tc_admission_t *why) {
  if (!p || p->cores < 1 || p->cores > 3 || p->patch.n_plugins < 0 ||
      p->patch.n_plugins >= GRAPH_MAX_NODES || p->patch.n_edges < 0 ||
      p->patch.n_edges > GRAPH_MAX_EDGES)
    return SESSION_EFORMAT;
  audio_graph_t g;
  audio_graph_init(&g, NULL);
  temporal_contract_t task[TC_MAX_TASKS];
  for (int i = 0; i < p->patch.n_plugins; ++i) {
    if (audio_graph_add_node(&g, (uint32_t)i + 1) < 0)
      return SESSION_EFORMAT;
    task[i] = p->contract[i];
    task[i].pid = (uint32_t)i + 1;
  }
  int dac = audio_graph_add_dac(&g);
  if (dac < 0)
    return SESSION_EFORMAT;
  for (int i = 0; i < p->patch.n_edges; ++i) {
    int a = p->patch.edges[i].src, b = p->patch.edges[i].dst;
    if (a < 0 || a >= p->patch.n_plugins ||
        (b != PATCH_DAC && (b < 0 || b >= p->patch.n_plugins)))
      return SESSION_EFORMAT;
    if (b == PATCH_DAC)
      b = dac;
    int e = p->feedback[i] ? audio_graph_connect_feedback(&g, a, b)
                           : audio_graph_connect(&g, a, b);
    if (e < 0)
      return SESSION_EFORMAT;
  }
  if (p->patch.n_params < 0 || p->patch.n_params > PATCH_MAX_PARAMS)
    return SESSION_EFORMAT;
  unsigned params[PATCH_MAX_PLUGINS] = {0};
  for (int i = 0; i < p->patch.n_params; ++i) {
    int node = p->patch.params[i].plugin;
    if (node < 0 || node >= p->patch.n_plugins || ++params[node] > 16u)
      return SESSION_EFORMAT;
    for (int j = 0; j < i; ++j)
      if (p->patch.params[j].plugin == node &&
          p->patch.params[j].id == p->patch.params[i].id)
        return SESSION_EFORMAT;
  }
  return tm_plan_build(&g, task, (unsigned)p->patch.n_plugins, limits,
                       (1u << (p->cores + 1)) - 2u, p->pin, out, why);
}
