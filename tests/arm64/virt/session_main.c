/* Common four-core QEMU application. SESSION_AUTOTEST adds acceptance checks;
 * without it, this is an interactive serial workstation over the same engine.
 * FAT storage is a persistent test block image, NOT an emulated I2S/SD driver.
 */
#include "audio_session.h"
#include "budget.h"
#include "exceptions.h"
#include "gic.h"
#include "m12_finish.h"
#include "mmu.h"
#include "pmem.h"
#include "pmm.h"
#include "sample_bits.h"
#include "session_shell.h"
#include "smp.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stddef.h>
#include <stdint.h>
void uart_virt_init(void);
extern char session_source_start[], session_source_end[], session_gain_start[],
    session_gain_end[];
extern char session_hog_start[], session_hog_end[], session_sine_start[],
    session_sine_end[];
#ifndef SESSION_RATE
#define SESSION_RATE 48000u
#endif
#define RATE SESSION_RATE
#ifndef SESSION_FRAMES
#define SESSION_FRAMES 64u
#endif
#define FRAMES SESSION_FRAMES
#define HZ (RATE / FRAMES)
#define SD_SECTORS 2048u
/* QMP exports this physical region between two independent emulator boots. */
uint8_t session_sd[SD_SECTORS * 512u]
    __attribute__((section(".session_sd"), aligned(4096)));
static audio_session_t app;
static audio_worker_t workers[3];
static uint8_t stacks[3][32768] __attribute__((aligned(16)));
static fat_fs_t filesystem;
static shell_t shell;
static shell_cmd_t commands[32];
static uint64_t hz, ticks, sequence, callbacks, fresh, missing, hash;
static int16_t pcm[FRAMES * 2];
static uint32_t quitting, quit_requested, verify_samples;
static uint64_t irq_worst, irq_overruns;
static int32_t last_left, last_right;
static uint32_t sample_seq;
static uint64_t wrong_samples;
static size_t baseline;
static uint64_t clock_now(void) {
  uint64_t t;
  __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(t)::"memory");
  return t;
}
static void require(int ok, const char *why) {
  if (ok)
    return;
  uart_printf("SESSION: FAIL (%s)\r\n", why);
  m12_finish();
}
static void enable_readonly_counter(void) {
  /* CNTKCTL.EL0VCTEN (bit 1). No EL0 timer-register write access. */
  uint64_t v;
  __asm__ volatile("mrs %0,cntkctl_el1" : "=r"(v));
  v |= 2u;
  __asm__ volatile("msr cntkctl_el1,%0; isb" ::"r"(v) : "memory");
}
static int sd_read(void *ctx, uint32_t lba, uint8_t *p) {
  (void)ctx;
  if (lba >= SD_SECTORS)
    return -1;
  for (unsigned j = 0; j < 512; ++j)
    p[j] = session_sd[lba * 512 + j];
  return 0;
}
static int sd_write(void *ctx, uint32_t lba, const uint8_t *p) {
  (void)ctx;
  if (lba >= SD_SECTORS)
    return -1;
  for (unsigned j = 0; j < 512; ++j)
    session_sd[lba * 512 + j] = p[j];
  return 0;
}
static void u16(uint8_t *p, unsigned v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static void storage(void) {
  int fresh_disk = session_sd[510] != 0x55 || session_sd[511] != 0xaa;
  if (fresh_disk) {
    for (unsigned j = 0; j < sizeof(session_sd); ++j)
      session_sd[j] = 0;
    u16(session_sd + 11, 512);
    session_sd[13] = 1;
    u16(session_sd + 14, 1);
    session_sd[16] = 1;
    u16(session_sd + 17, 64);
    u16(session_sd + 19, SD_SECTORS);
    u16(session_sd + 22, 8);
    session_sd[510] = 0x55;
    session_sd[511] = 0xaa;
  }
  require(!fat_mount(&filesystem, sd_read, NULL), "FAT mount");
  fat_set_writer(&filesystem, sd_write);
  if (fresh_disk) {
    require(fat_write_file(
                &filesystem, "SOURCE.ELF", (uint8_t *)session_source_start,
                (uint32_t)(session_source_end - session_source_start)) > 0,
            "source file");
    require(
        fat_write_file(&filesystem, "GAIN.ELF", (uint8_t *)session_gain_start,
                       (uint32_t)(session_gain_end - session_gain_start)) > 0,
        "gain file");
    require(fat_write_file(&filesystem, "HOG.ELF", (uint8_t *)session_hog_start,
                           (uint32_t)(session_hog_end - session_hog_start)) > 0,
            "hog file");
    require(
        fat_write_file(&filesystem, "SYNTH.ELF", (uint8_t *)session_sine_start,
                       (uint32_t)(session_sine_end - session_sine_start)) > 0,
        "synth file");
  }
  session_mount(&app, &filesystem);
}
void scheduler_tick(struct trapframe *tf) {
  (void)tf;
  uint64_t entry = clock_now();
  int had_frame = app.runtime.started && !(app.runtime.gate & 2u);
  int got =
      session_tick(&app, ++sequence, timer_deadline() - timer_interval(), pcm);
  if (got) {
    __atomic_fetch_add(&fresh, 1, __ATOMIC_RELAXED);
    uint64_t h = 2166136261u;
    for (unsigned j = 0; j < FRAMES * 2; ++j) {
      h = (h ^ (uint16_t)pcm[j]) * 16777619u;
      if (__atomic_load_n(&verify_samples, __ATOMIC_RELAXED) &&
          pcm[j] != (j & 1 ? -8192 : 8192))
        __atomic_fetch_add(&wrong_samples, 1, __ATOMIC_RELAXED);
    }
    uint32_t seq = __atomic_load_n(&sample_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&sample_seq, seq + 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&hash, h, __ATOMIC_SEQ_CST);
    __atomic_store_n(&last_left, pcm[0], __ATOMIC_SEQ_CST);
    __atomic_store_n(&last_right, pcm[1], __ATOMIC_SEQ_CST);
    __atomic_store_n(&sample_seq, seq + 2, __ATOMIC_SEQ_CST);
  } else if (had_frame)
    __atomic_fetch_add(&missing, 1, __ATOMIC_RELAXED);
  uint64_t elapsed = clock_now() - entry;
  if (elapsed > irq_worst)
    __atomic_store_n(&irq_worst, elapsed, __ATOMIC_RELAXED);
  if (elapsed > ticks / 2)
    __atomic_fetch_add(&irq_overruns, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&callbacks, 1, __ATOMIC_RELEASE);
  /* Wake the serial observer after publishing the cadence count. Idle
   * control must not burn a host/physical core merely waiting for audio. */
  __asm__ volatile("dsb ishst; sev" ::: "memory");
}

static void worker_main(void *ctx) {
  mmu_join();
  exceptions_init();
  gic_cpu_init();
  enable_readonly_counter();
  aw_worker_loop(ctx);
}
static void output(void *ctx, const char *s) {
  (void)ctx;
  uart_puts(s);
}
static int wait_cmd(shell_t *sh, int n, char **v) {
  (void)sh;
  uint64_t count;
  if (n != 2 || session_parse_u64(v[1], &count) || !count || count > 10000)
    return TC_EINVAL;
  uint64_t end = __atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) + count;
  uint64_t timeout = clock_now() + hz * 20;
  while (__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) < end) {
    if (clock_now() >= timeout)
      return SESSION_ETIMEOUT;
#ifdef SESSION_AUTOTEST
    __asm__ volatile("wfi" ::: "memory"); /* CPU0 owns the cadence IRQ */
#else
    __asm__ volatile("wfe" ::: "memory"); /* CPU3 wakes after published frame */
#endif
  }
  return 0;
}
static int inspect_cmd(shell_t *sh, int n, char **v) {
  (void)sh;
  (void)v;
  if (n != 1)
    return TC_EINVAL;
  uint64_t f = __atomic_load_n(&fresh, __ATOMIC_RELAXED),
           m = __atomic_load_n(&missing, __ATOMIC_RELAXED), h = 0;
  int left = 0, right = 0;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    uint32_t seq = __atomic_load_n(&sample_seq, __ATOMIC_SEQ_CST);
    if (seq & 1u)
      continue;
    h = __atomic_load_n(&hash, __ATOMIC_SEQ_CST);
    left = __atomic_load_n(&last_left, __ATOMIC_SEQ_CST);
    right = __atomic_load_n(&last_right, __ATOMIC_SEQ_CST);
    if (seq == __atomic_load_n(&sample_seq, __ATOMIC_SEQ_CST))
      break;
  }
  uart_printf("samples: fresh=%u missing=%u hash=%x left=%d right=%d "
              "watchdog=%u swaps=%u\r\n",
              (unsigned)f, (unsigned)m, (unsigned)h, left, right,
              (unsigned)__atomic_load_n(&irq_overruns, __ATOMIC_RELAXED),
              (unsigned)__atomic_load_n(&app.swaps, __ATOMIC_RELAXED));
  uart_printf("scheduler: accepted=%u skipped=%u invalid=%u last=%d gate=%u frame=%u callback=%u\r\n",(unsigned)app.runtime.accepted,(unsigned)app.runtime.skipped,(unsigned)app.invalid_kicks,app.last_kick_result,app.runtime.gate,(unsigned)app.runtime.frame,(unsigned)sequence);
  for(unsigned i=0;i<3;++i) uart_printf("worker%u: sent=%u done=%u blocks=%u\r\n",i+1,(unsigned)workers[i].block_seq,(unsigned)workers[i].done_seq,(unsigned)workers[i].blocks);
  return 0;
}
static int quit_cmd(shell_t *sh, int n, char **v) {
  (void)sh;
  (void)v;
  if (n != 1)
    return TC_EINVAL;
  quit_requested = 1;
  return 0;
}
static void initialize(void) {
  uart_virt_init();
  uart_puts("Tessera M11/M13 workstation\r\n");
  pmm_init();
  mmu_init();
  exceptions_init();
  gic_init();
  enable_readonly_counter();
  __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(hz));
  ticks = hz / HZ;
  uart_printf("audio format: sample_rate=%u frames=%u\r\n", RATE, FRAMES);
  audio_worker_t *ws[3];
  for (unsigned i = 0; i < 3; ++i) {
    aw_init(&workers[i], i + 1);
    ws[i] = &workers[i];
  }
  tm_limits_t limits = {
      {ticks, hz / 10000, hz / 25000},
      hz / 50000}; /* 100us frame, 40us input/output, 20us handoff */
  require(session_init(&app, ws, &limits, clock_now, hz, RATE, FRAMES, hz / 20,
#ifdef SESSION_AUTOTEST
                       14
#else
                       6
#endif
                       ) == TC_OK,
          "session init");
  storage();
  baseline = pmm_free_pages();
  unsigned dsp_cores = app.available_mask == 14 ? 3u : 2u;
  for (unsigned i = 0; i < dsp_cores; ++i)
    require(!smp_start_core(i + 1, worker_main, ws[i],
                            (uintptr_t)(stacks[i] + sizeof(stacks[i]))),
            "worker start");
  uint64_t start = clock_now();
  for (unsigned i = 0; i < dsp_cores; ++i) {
    while (!__atomic_load_n(&workers[i].online, __ATOMIC_ACQUIRE) &&
           clock_now() - start < hz) {
    }
    require(workers[i].online, "worker online");
  }
  session_shell_init(&shell, &app, output, NULL);
  int n = shell.n_cmds;
  require(n + 3 < 32, "command capacity");
  for (int i = 0; i < n; ++i)
    commands[i] = shell.cmds[i];
  commands[n++] =
      (shell_cmd_t){"wait", "wait <callbacks> (QEMU observation)", wait_cmd};
  commands[n++] = (shell_cmd_t){"inspect", "last PCM block (QEMU observation)",
                                inspect_cmd};
  commands[n++] = (shell_cmd_t){"quit", "stop emulator", quit_cmd};
  shell.cmds = commands;
  shell.n_cmds = n;
  timer_init(HZ);
  __asm__ volatile("msr daifclr,#2");
}
#ifdef SESSION_AUTOTEST
static temporal_contract_t contract(uint64_t budget) {
  return (temporal_contract_t){.period = ticks,
                               .deadline = ticks,
                               .cpu_budget = budget,
                               .criticality = TC_HARD,
                               .overrun_policy = TC_MUTE_THEN_KILL,
                               .kill_after = 3};
}
static void frames(unsigned count) {
  char b[24];
  unsigned k = 0;
  char rev[12];
  do {
    rev[k++] = (char)('0' + count % 10);
    count /= 10;
  } while (count);
  unsigned j = 0;
  while (k)
    b[j++] = rev[--k];
  b[j] = 0;
  char *args[] = {"wait", b};
  require(!wait_cmd(&shell, 2, args), "frame wait");
}
static void automatic(void) {
  /* The same 40%-of-frame DSP work exceeds a frame when run serially.
   * Reserve 60% per core including cold EL0/IRQ entry in MTTCG; this is
   * an explicit test allowance, not a measured hardware WCET. */
  temporal_contract_t heavy = contract(ticks * 3 / 5), light = contract(ticks / 8);
  uint32_t pids[3];
  for (unsigned i = 0; i < 3; ++i) {
    long p = session_load(&app, "/sd/SOURCE.ELF", &heavy);
    require(p > 0, "heavy load");
    pids[i] = (uint32_t)p;
    uint32_t bits = sample_float(
        0); /* replaced by exact integer-to-float conversion below */
    char duration[16], reverse[16];
    unsigned length = 0, count = 0;
    uint64_t microseconds = (ticks * 1000000 / hz) * 2 / 5;
    do {
      reverse[length++] = (char)('0' + microseconds % 10);
      microseconds /= 10;
    } while (microseconds);
    while (length)
      duration[count++] = reverse[--length];
    duration[count] = 0;
    require(!patch_parse_value(duration, &bits), "delay value");
    require(!session_set_param(&app, pids[i], 1, bits), "delay set");
  }
  require(session_set_cores(&app, 1) == TC_EADMISSION, "serial graph rejected");
  require(app.scene[app.active].cores == 3, "core rejection transactional");
  for (unsigned i = 0; i < 3; ++i)
    require(app.scene[app.active].plan.core[i] == i + 1,
            "automatic distinct placement");
  /* Measure identical work serially while stopped, without overriding normal
   * execution ownership. Clear host references only for this benchmark. */
  /* The intentionally serial, IRQ-masked EL0 benchmark is NOT an audio
   * engine run. Do not carry its delayed cadence IRQs into the parallel
   * acceptance window. Both measurements still use identical DSP work. */
  timer_stop();
  __asm__ volatile("msr daifset,#2");
  uint64_t serial_start = clock_now();
  for (unsigned i = 0; i < 3; ++i) {
    session_node_t *node = app.scene[app.active].nodes[i];
    plugin_t *pl = node->plugin;
    __atomic_store_n(&pl->host_refs, 0, __ATOMIC_RELEASE);
    unsigned cpu = pl->bound_cpu;
    pl->bound_cpu = 0;
    require(plugin_call_block(pl, SESSION_IN_VA, SESSION_IN_VA + FRAMES * 4,
                              SESSION_OUT_VA, SESSION_OUT_VA + FRAMES * 4,
                              FRAMES) >= 0,
            "serial work");
    pl->bound_cpu = cpu;
    __atomic_store_n(&pl->host_refs, 1, __ATOMIC_RELEASE);
  }
  uint64_t serial = clock_now() - serial_start;
  require(serial > ticks, "serial measured deadline violation");
  timer_init(HZ);
  __asm__ volatile("msr daifclr,#2");
  require(!session_start(&app), "parallel start");
  frames(129);
  require(!session_pause(&app), "parallel pause");
  for (unsigned i = 0; i < 3; ++i) {
    tc_task_state_t st;
    require(!tm_snapshot(&app.runtime, pids[i], &st), "parallel stats");
    uart_printf(
        "capacity pid=%u runs=%u completed=%u budget=%u deadline=%u max=%uus "
        "skipped=%u fresh=%u\r\n",
        pids[i], (unsigned)st.runs, (unsigned)st.completed,
        (unsigned)st.budget_overruns, (unsigned)st.deadline_misses,
        (unsigned)(st.service_max * 1000000 / hz),
        (unsigned)app.runtime.skipped, (unsigned)fresh);
    require(st.completed >= 128 && !st.budget_overruns && !st.deadline_misses,
            "parallel completion");
  }
  require(!app.runtime.skipped, "parallel zero skips");
  uart_printf("M11 capacity: serial=%uus frame=%uus; three cores >=128 jobs "
              "each, zero misses\r\n",
              (unsigned)(serial * 1000000 / hz),
              (unsigned)(ticks * 1000000 / hz));
  require(!session_clear(&app) && pmm_free_pages() == baseline,
          "capacity cleanup");
  long source = session_load(&app, "/sd/SOURCE.ELF", &light),
       gain = session_load(&app, "/sd/GAIN.ELF", &light);
  require(source > 0 && gain > 0, "chain load");
  require(!session_wire(&app, (uint32_t)source, (uint32_t)gain, 0) &&
              !session_wire(&app, (uint32_t)gain, 0, 0),
          "chain wiring");
  require(!session_pin(&app, (uint32_t)source, 3) &&
              !session_pin(&app, (uint32_t)gain, 1),
          "cross-core pins");
  require(app.scene[app.active].plan.cross_edges == 1, "cross edge counted");
  uint64_t f = fresh;
  wrong_samples = 0;
  verify_samples = 1;
  require(!session_start(&app), "chain start");
  frames(129);
  require(!session_pause(&app) && fresh - f >= 128 && !wrong_samples,
          "cross-core actual samples");
  verify_samples = 0;
  require(!session_wire(&app, (uint32_t)gain, (uint32_t)source, 1),
          "feedback admitted");
  require(!session_unwire(&app, (uint32_t)gain, (uint32_t)source),
          "feedback removed");
  require(!session_save(&app, "/sd/LIVE.TSP"), "persist contracts");
  session_preset_t preset;
  require(!session_capture(&app, &preset), "capture scene");
  require(session_restore(&app, "/sd/MISSING.TSP") < 0 &&
              app.scene[app.active].count == 2,
          "missing restore retained");
  require(!session_clear(&app), "clear before restore");
  require(!session_restore(&app, "/sd/LIVE.TSP"), "restore scene");
  session_preset_t restored;
  require(!session_capture(&app, &restored), "restored capture");
  require(restored.patch.n_plugins == 2 && restored.patch.n_edges == 2 &&
              restored.pin[0] == 3 && restored.pin[1] == 1,
          "restored affinity graph");
  require(!session_start(&app), "restored run");
  wrong_samples = 0;
  verify_samples = 1;
  frames(65);
  require(!session_pause(&app) && !wrong_samples, "restored audio");
  verify_samples = 0;
  source = app.scene[app.active].nodes[0]->pid;
  gain = app.scene[app.active].nodes[1]->pid;
  for (unsigned cpu = 1; cpu <= 3; ++cpu) {
    long hog = session_load(&app, "/sd/HOG.ELF", &light);
    require(hog > 0, "hog load");
    require(!session_pin(&app, (uint32_t)hog, cpu), "hog pin");
    wrong_samples = 0;
    verify_samples = 1;
    require(!session_start(&app), "fault graph start");
    frames(33);
    require(!session_pause(&app), "fault graph pause");
    verify_samples = 0;
    tc_task_state_t st;
    int got_state=tm_snapshot(&app.runtime,(uint32_t)hog,&st);
    uart_printf("hog cpu=%u pid=%u snapshot=%d runs=%u complete=%u killed=%u budget=%u deadline=%u faults=%u streak=%u\r\n",cpu,(unsigned)hog,got_state,(unsigned)st.runs,(unsigned)st.completed,st.killed,(unsigned)st.budget_overruns,(unsigned)st.deadline_misses,(unsigned)st.faults,st.streak);
    require(!got_state && st.killed && st.budget_overruns==3,"hog terminated on each core");
    require(!wrong_samples && !app.runtime.skipped,
            "fault did not corrupt chain");
    require(!session_unload(&app, (uint32_t)hog), "hog cleanup");
  }
  require(!session_clear(&app) && pmm_free_pages() == baseline,
          "final frame baseline");
  uart_puts("M11 dependencies: cross-core actual stereo + fault on CPUs1/2/3 "
            "PASS\r\n");
  uart_puts("M13 session: FAT save/load contracts/affinity/audio PASS\r\n");
  uart_puts("SESSION: PASS\r\n");
}
#endif

#ifndef SESSION_AUTOTEST
static void console_main(void *ctx) {
  (void)ctx;
  mmu_join();
  exceptions_init();
  gic_cpu_init();
  enable_readonly_counter();
  uart_puts("control: CPU3; audio cadence: CPU0; DSP: CPU1/2\r\n");
  shell_prompt(&shell);
  while (!quit_requested) {
    int c = uart_getc();
    if (c >= 0)
      shell_feed(&shell, (char)c);
    else
      __asm__ volatile("wfe" ::: "memory");
  }
  require(!session_pause(&app), "console pause");
  require(!session_clear(&app), "interactive cleanup");
  require(pmm_free_pages() == baseline, "interactive frame baseline");
  uart_puts("WORKSTATION: PASS\r\n");
  __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
  __asm__ volatile("sev");
}
#endif
void test_main(void) {
  initialize();
#ifdef SESSION_AUTOTEST
  automatic();
#else
  require(!smp_start_core(3, console_main, NULL,
                          (uintptr_t)(stacks[2] + sizeof(stacks[2]))),
          "console CPU3 start");
  while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE))
    __asm__ volatile("wfi");
#endif
  timer_stop();
  __asm__ volatile("msr daifset,#2");
  for (unsigned i = 0; i < 3; ++i)
    aw_stop(&workers[i]);
  m12_finish();
}
