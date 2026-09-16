/* Strict M11 acceptance for the shared session engine (included by the QEMU
 * application with SESSION_M11). Every measured window is exact: no tolerated
 * missed blocks, retries, or hardware timing claims. The default fixture uses
 * an explicit 12 kHz / 240-sample = 20 ms QEMU profile so host scheduling
 * jitter cannot masquerade as a DSP deadline failure. MTTCG JIT warm-up is
 * reported separately before the capacity measurements. */
static void window(unsigned count) {
  require(count > 1, "window size");
  uint64_t before = callbacks, overruns = irq_overruns;
  window_end = before + count;
  timer_init(HZ);
  __asm__ volatile("msr daifclr,#2" ::: "memory");
  uint64_t timeout = clock_now() + hz * 10;
  while (__atomic_load_n(&callbacks, __ATOMIC_ACQUIRE) < window_end) {
    require(clock_now() < timeout, "callback window timeout");
    __asm__ volatile("yield" ::: "memory");
  }
  __asm__ volatile("msr daifset,#2" ::: "memory");
  require(callbacks - before == count && timer_ticks() == count,
          "every CPU0 callback serviced exactly once");
  require(irq_overruns == overruns, "zero CPU0 watchdog overruns");
  /* Keep the trace window open until its last worker has returned. */
  timeout = clock_now() + hz;
  while (!tm_drained(&app.runtime))
    require(clock_now() < timeout, "workers drain after window");
  window_end = 0;
}
static tc_task_state_t snapshot(uint32_t pid) {
  tc_task_state_t t;
  require(tm_snapshot(&app.runtime, pid, &t) == TC_OK, "stable task snapshot");
  return t;
}
static void completed(uint32_t pid, tc_task_state_t before, unsigned count) {
  tc_task_state_t after = snapshot(pid);
  if (after.completed - before.completed != count ||
      after.deadline_misses != before.deadline_misses)
    uart_printf("M11 task pid=%u releases=%u runs=%u complete=%u deadline=%u "
                "budget=%u missed=%u expected=%u max=%uus\r\n", pid,
                (unsigned)(after.releases-before.releases),
                (unsigned)(after.runs-before.runs),
                (unsigned)(after.completed-before.completed),
                (unsigned)(after.deadline_misses-before.deadline_misses),
                (unsigned)(after.budget_overruns-before.budget_overruns),
                (unsigned)(after.missed_releases-before.missed_releases), count,
                (unsigned)(after.service_max*1000000/hz));
  require(after.releases - before.releases == count &&
              after.runs - before.runs == count &&
              after.completed - before.completed == count,
          "healthy plugin produced on EVERY release");
  require(after.budget_overruns == before.budget_overruns &&
              after.deadline_misses == before.deadline_misses &&
              after.missed_releases == before.missed_releases &&
              after.shed == before.shed && after.faults == before.faults &&
              !after.killed,
          "healthy plugin zero offences/misses/shed/faults");
}
static void clean_run(const uint32_t *pids, unsigned n, unsigned count) {
  tc_task_state_t before[TC_MAX_TASKS];
  uint64_t old_fresh = fresh, old_missing = missing;
  uint64_t old_skips = app.runtime.skipped, old_invalid = app.invalid_kicks;
  for (unsigned i = 0; i < n; ++i) before[i] = snapshot(pids[i]);
  require(session_start(&app) == TC_OK, "admitted start");
  window(count);
  require(session_pause(&app) == TC_OK, "drained pause");
  for (unsigned i = 0; i < n; ++i) completed(pids[i], before[i], count);
  require(fresh - old_fresh == count - 1 && missing == old_missing &&
              app.runtime.skipped == old_skips && app.invalid_kicks == old_invalid,
          "exact PCM frame count, zero missing/skipped/invalid kicks");
}
static long serial_call(void *ctx) {
  session_node_t *n = ctx;
  return plugin_call_block(n->plugin, SESSION_IN_VA,
                           SESSION_IN_VA + FRAMES * 4u, SESSION_OUT_VA,
                           SESSION_OUT_VA + FRAMES * 4u, FRAMES);
}
static uint32_t serial_errors;
static void serial_node(void *ctx) {
  /* Test-only negative control: bypass ADMISSION, not isolation/ownership.
   * Each actual EL0 job retains its budget and host reference. Deliberately
   * pack three 40%-frame jobs onto CPU1; the normal API rejects this plan. */
  if (budget_call(serial_call, ctx, ticks) < 0)
    __atomic_fetch_add(&serial_errors, 1u, __ATOMIC_RELAXED);
}
static void capacity(void) {
  temporal_contract_t heavy = contract(ticks * 3 / 5);
  uint32_t pids[3];
  for (unsigned i = 0; i < 3; ++i) {
    long pid = session_load(&app, "/sd/SOURCE.ELF", &heavy);
    require(pid > 0, "capacity ELF load");
    pids[i] = (uint32_t)pid;
  }
  /* Warm the translated kernel/EL0 paths with zero DSP delay. Warm-up
   * counters are retained and reported, not silently reclassified as passes. */
  require(!session_start(&app), "warm-up start");
  window(8);
  require(!session_pause(&app), "warm-up pause");
  unsigned warm_misses = 0;
  for (unsigned i = 0; i < 3; ++i) {
    tc_task_state_t t = snapshot(pids[i]);
    require(!t.killed && !t.budget_overruns, "warm-up survived");
    warm_misses += (unsigned)t.deadline_misses;
    /* 8 ms = 40% of the 20 ms acceptance frame.  Three identical jobs need
     * 24 ms serially but only 8 ms each when placed on CPU1/2/3. */
    require(!session_set_param(&app, pids[i], 1, 0x45fa0000u),
            "same 8000us DSP workload on each core");
  }
  uart_printf("M11 warmup: frames=8 deadline_misses=%u (not capacity evidence)\r\n",
              warm_misses);
  require(session_set_cores(&app, 1) == TC_EADMISSION &&
              app.scene[app.active].cores == 3,
          "production admission rejects overload transactionally");
  void (*saved_run)(void *) = workers[0].nodes[0].run;
  void *saved_ctx = workers[0].nodes[0].ctx;
  require(tm_paused(&app.runtime), "unadmitted test starts drained");
  aw_clear(&workers[0]);
  for (unsigned i = 0; i < 3; ++i) {
    session_node_t *n = app.scene[app.active].nodes[i];
    n->plugin->bound_cpu = 1;
    require(aw_assign(&workers[0], serial_node, n) >= 0, "serial assignment");
  }
  uint64_t old_overruns = workers[0].overruns;
  uint64_t old_blocks = workers[0].blocks;
  serial_mode = 1;
  window(48);
  serial_mode = 0;
  uint64_t overruns = workers[0].overruns - old_overruns;
  require(overruns > 0 && workers[0].blocks - old_blocks + overruns == 48 &&
              !serial_errors, "same real EL0 work overruns one worker");
  for (unsigned i = 0; i < 3; ++i)
    require(workers[0].nodes[i].overruns == overruns,
            "overload attributed to every assigned node");
  aw_clear(&workers[0]);
  require(aw_assign(&workers[0], saved_run, saved_ctx) == 0, "restore session worker");
  for (unsigned i = 0; i < 3; ++i)
    app.scene[app.active].nodes[i]->plugin->bound_cpu =
        app.scene[app.active].plan.core[i];
  uint64_t blocks[3];
  for (unsigned i = 0; i < 3; ++i) {
    require(app.scene[app.active].plan.core[i] == i + 1, "CPU1/2/3 placement");
    blocks[i] = workers[i].blocks;
  }
  clean_run(pids, 3, 129);
  for (unsigned i = 0; i < 3; ++i)
    require(workers[i].blocks - blocks[i] == 129, "every worker executes every frame");
  uart_printf("M11 capacity: single_core_overruns=%u/48; parallel=129/129 "
              "per CPU1/2/3; missing=0 skips=0 deadline=0 watchdog=0\r\n",
              (unsigned)overruns);
  require(!session_clear(&app) && pmm_free_pages() == baseline, "capacity no leak");
}
static void filter_equivalence(void) {
  temporal_contract_t light = contract(ticks / 5);
  for (unsigned split = 0; split < 2; ++split) {
    long source = session_load(&app, "/sd/SOURCE.ELF", &light);
    long filter = session_load(&app, "/sd/FILTER.ELF", &light);
    require(source > 0 && filter > 0, "real low-pass chain load");
    uint32_t pids[] = {(uint32_t)source, (uint32_t)filter};
    require(!session_wire(&app, pids[0], pids[1], 0) &&
                !session_wire(&app, pids[1], 0, 0), "real low-pass wiring");
    require(!session_pin(&app, pids[0], split ? 3 : 1) &&
                !session_pin(&app, pids[1], 1), "single/split chain placement");
    require(app.scene[app.active].plan.cross_edges == split, "same-frame edge count");
    capture_mode = split ? 2 : 1;
    captured = 0;
    clean_run(pids, 2, 65);
    capture_mode = 0;
    require(captured == 64 && !wrong_samples, "every PCM bit identical including filter transient");
    require(!session_clear(&app) && pmm_free_pages() == baseline, "chain no leak");
  }
  uart_puts("M11 topology: real low-pass single CPU1 vs CPU3->CPU1; "
            "30720 PCM16 samples bit-identical; same-frame edges PASS\r\n");
}
static void hex64(uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  uart_puts("0x");
  for (int i = 15; i >= 0; --i) uart_putc(digits[(v >> (i * 4)) & 15]);
}
static void fault_cycles(void) {
  /* MTTCG service includes host descheduling. At the 20 ms acceptance period
   * survivors reserve 6 ms and the hostile job gets a separate 2.5 ms cap.
   * Admission still includes every cost, and any miss/offence still fails. */
  temporal_contract_t light = contract(ticks * 3 / 10);
  temporal_contract_t hostile = contract(ticks / 8);
  uint32_t good[3];
  for (unsigned i = 0; i < 3; ++i) {
    long pid = session_load(&app, i == 1 ? "/sd/GAIN.ELF" : "/sd/SOURCE.ELF", &light);
    require(pid > 0, "survivor load");
    good[i] = (uint32_t)pid;
    require(!session_pin(&app, good[i], i + 1), "survivor on each CPU");
  }
  require(!session_wire(&app, good[0], good[1], 0) &&
              !session_wire(&app, good[1], 0, 0), "survivor DAC chain");
  size_t live_baseline = pmm_free_pages();
  verify_samples = 1;
  for (unsigned cycle = 0; cycle < 3; ++cycle)
    for (unsigned cpu = 1; cpu <= 3; ++cpu)
      for (unsigned crash = 0; crash < 2; ++crash) {
        long pid = session_load(&app, crash ? "/sd/CRASH.ELF" : "/sd/HOG.ELF", &hostile);
        require(pid > 0 && !session_pin(&app, (uint32_t)pid, cpu), "fault load/pin");
        clean_run(good, 3, 33);
        tc_task_state_t bad = snapshot((uint32_t)pid);
        require(bad.killed && !bad.deadline_misses && !bad.missed_releases,
                "fault contained without dispatch misses");
        session_node_t *n = app.scene[app.active].nodes[3];
        require(n->pid == (uint32_t)pid && n->plugin->proc->state == PROC_KILLED,
                "process death published");
        if (crash) {
          process_fault_t f;
          require(bad.faults == 1 && bad.runs == 4 && bad.completed == 3 &&
                      !bad.budget_overruns, "real mid-run crash, no reentry");
          require(process_fault_snapshot(n->plugin->proc, &f) && f.cpu == cpu &&
                      (f.esr >> 26) == EC_DATA_ABORT_L && f.far == 0 &&
                      (f.esr & (1u << 6)) && f.elr >= USER_VA_BASE,
                  "retained real fault core, syndrome, address and PC");
          uart_printf("M11 fault cycle=%u cpu=%u pid=%u ESR=", cycle + 1, cpu, (unsigned)pid);
          hex64(f.esr); uart_puts(" ELR="); hex64(f.elr);
          uart_puts(" FAR="); hex64(f.far); uart_puts(" SPSR="); hex64(f.spsr);
          uart_puts("\r\n");
        } else {
          require(bad.budget_overruns == 3 && !bad.faults && bad.runs == 6 &&
                      bad.completed == 3, "hog killed after exactly three offences");
        }
        for (unsigned j = 0; j < FRAMES * 2; ++j)
          require(n->output[app.runtime.bank][j] == 0, "partial hostile output erased");
        require(!wrong_samples, "survivor PCM unchanged during fault");
        require(!session_unload(&app, (uint32_t)pid) &&
                    pmm_free_pages() == live_baseline, "EVERY fault cycle no leak");
      }
  verify_samples = 0;
  uart_puts("M11 resilience: 18 load/fault/unload cycles; crashes+CPU hogs on "
            "CPU1/2/3; survivors=594/594 each; zero PCM loss and per-cycle leaks\r\n");
  require(!session_clear(&app) && pmm_free_pages() == baseline, "final frame baseline");
  uint64_t parked[3];
  for (unsigned i = 0; i < 3; ++i) parked[i] = workers[i].blocks;
  /* Empty committed scene has no DSP work and never wakes an empty worker. */
  require(tm_resume(&app.runtime) == TC_OK, "empty runtime resume");
  for (unsigned f = 0; f < 4; ++f) {
    uint64_t seq = ++sequence, now = clock_now();
    if (f) now = app.runtime.release + ticks;
    while (clock_now() < now) __asm__ volatile("yield");
    require(tm_kick(&app.runtime, seq, now) == 1, "empty frame publication");
  }
  tm_request_pause(&app.runtime);
  for (unsigned i = 0; i < 3; ++i)
    require(workers[i].blocks == parked[i], "empty worker stays parked");
}
static void automatic(void) {
  timer_stop();
  __asm__ volatile("msr daifset,#2" ::: "memory");
  require(RATE == 12000 && FRAMES == 240 && HZ == 50,
          "12kHz/240 acceptance configuration");
  capacity();
  filter_equivalence();
  fault_cycles();
  require(!cpu0_svcs && !plugin_syscalls, "zero CPU0 SVC and plugin-body syscalls");
  uart_puts("M11 realtime: zero CPU0 SVC; zero plugin-body syscalls "
            "(bounded trampoline EXIT is explicit); empty workers parked\r\n");
  uart_puts("MULTICORE: PASS\r\n");
}
