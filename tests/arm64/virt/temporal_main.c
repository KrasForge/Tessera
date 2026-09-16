/* Real AArch64/EL0 temporal-contract acceptance. CPU0 cadence, CPU1 host.
 * 200 Hz is a functional stress cadence, NOT a hardware audio-latency claim. */
#include "pmm.h"
#include "pmem.h"
#include "mmu.h"
#include "vmem.h"
#include "process.h"
#include "exceptions.h"
#include "plugin_loader.h"
#include "plugin_abi.h"
#include "plugin_mgr.h"
#include "temporal_host.h"
#include "ring_contract.h"
#include "m12_finish.h"
#include "smp.h"
#include "spsc_ring.h"
#include "audio_core.h"
#include "gic.h"
#include "timer.h"
#include "uart_pl011.h"
#include <stdint.h>
#include <stddef.h>

void uart_virt_init(void);
extern char good_elf_start[], good_elf_end[], blip_elf_start[], blip_elf_end[];
extern char trampoline_svc_start[], trampoline_svc_end[];
extern char hog_elf_start[], hog_elf_end[], temporal_ctl_start[], temporal_ctl_end[];
#define HZ 200u
#define BLOCKS 100u
#define COUNT 8u
#define FRAMES RING_BLOCK
#define SAMPLES (2u * FRAMES)
enum { GOOD, BLIP, KILL, STRIKES, MUTE, BYPASS, DEGRADE, BEST };
static const char *names[] = {"good", "blip", "kill", "strikes", "mute", "bypass", "degrade", "best"};
static const unsigned policies[] = {TC_MUTE, TC_BYPASS, TC_KILL, TC_MUTE_THEN_KILL,
                                     TC_MUTE, TC_BYPASS, TC_DEGRADE, TC_BYPASS};
static uint64_t clock_now(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
static void require(int ok, const char *what)
{
    if (ok) return;
    uart_printf("TEMPORAL: FAIL (%s)\r\n", what);
    m12_finish();
    for (;;) __asm__ volatile("wfe");
}
static void *ring_new(void *c) { (void)c; return NULL; }
static void ring_del(void *c, void *r) { (void)c; (void)r; }
static int ring_map(void *c, uint32_t p, void *r, int i)
{ (void)c; (void)p; (void)r; (void)i; return 0; }
static void ring_unmap(void *c, uint32_t p, void *r, int i)
{ (void)c; (void)p; (void)r; (void)i; }
static graph_control_t gc;
static plugin_mgr_t pm;
static temporal_host_t host;
static audio_worker_t worker;
static spsc_ring_t dac;
static int16_t dac_buf[4096], dma[SAMPLES];
static audio_core_t audio;
static uint64_t seq, freq, interval;
static volatile uint64_t underruns;
static uint32_t publish_order[COUNT], published;
static uint8_t stack1[24576] __attribute__((aligned(16)));
typedef struct {
    plugin_t *pl;
    uint32_t *out, *in;
    uint32_t pid, index, calls, sound, silent, dry, invalid, liveness;
} node_t;
static node_t node[COUNT];

static void map_io(node_t *n, plugin_t *pl)
{
    n->pl = pl;
    n->pid = pl->proc->pid;
    uintptr_t a = phys_alloc_page_zero(), b = phys_alloc_page_zero();
    require(a && b, "I/O allocation");
    require(!plugin_map_region(pl, RESULTS_VA, a, PAGE_SIZE, VMM_READ | VMM_WRITE), "output mapping");
    require(!plugin_map_region(pl, RING_IN_VA, b, PAGE_SIZE, VMM_READ), "input mapping");
    n->out = (uint32_t *)P2V(a); n->in = (uint32_t *)P2V(b);
    for (unsigned i = 0; i < SAMPLES; ++i) n->in[i] = 0x3d000000u + i;
}
static budget_plugin_call_t call_for(node_t *n)
{
    return (budget_plugin_call_t){n->pl, RING_IN_VA, RING_IN_VA + FRAMES * 4u,
        RESULTS_VA, RESULTS_VA + FRAMES * 4u, n->out, n->out + FRAMES, FRAMES};
}
static int has_sound(const node_t *n)
{
    for (unsigned i = 0; i < SAMPLES; ++i) if (n->out[i]) return 1;
    return 0;
}
static void publish(void *ctx, uint32_t pid, enum tc_output action)
{
    node_t *n = ctx;
    ++n->calls;
    if (published < COUNT) publish_order[published++] = pid;
    if (action == TC_OUTPUT_OK) {
        if (has_sound(n)) ++n->sound;
        else ++n->invalid;
        if (n->index == GOOD) {
            int16_t samples[SAMPLES];
            for (unsigned i = 0; i < SAMPLES; ++i) samples[i] = (int16_t)(n->calls + i);
            if (spsc_write(&dac, samples, SAMPLES) != SAMPLES) ++n->invalid;
        }
    } else if (action == TC_OUTPUT_SILENCE) {
        ++n->silent;
        if (has_sound(n)) ++n->invalid;
    } else {
        ++n->dry;
        for (unsigned i = 0; i < SAMPLES; ++i)
            if (n->out[i] != n->in[i]) { ++n->invalid; break; }
    }
}
void scheduler_tick(struct trapframe *tf)
{
    (void)tf;
    if (audio.serviced >= BLOCKS) return;
    /* irq.c has already advanced the compare register to the NEXT tick. */
    aw_kick_at(&worker, ++seq, timer_deadline() - timer_interval());
    uint64_t t = clock_now();
    if (audio_core_fill(&audio) < SAMPLES) ++underruns;
    audio_wd_account(&audio.wd, clock_now() - t);
    ++audio.serviced;
}
static void worker_entry(void *arg)
{
    mmu_join(); exceptions_init(); gic_cpu_init(); gic_enable_irq(TIMER_IRQ);
    aw_worker_loop(arg);
}

/* Prove that absolute deadlines, not only relative budgets, preempt EL0. */
static void cutoff_probe(void)
{
    plugin_t p; node_t n = {0};
    require(plugin_load(&p, hog_elf_start, (size_t)(hog_elf_end - hog_elf_start), "cutoff") == PLUGIN_OK, "probe load");
    map_io(&n, &p);
    require(plugin_call_init(&p, RING_SR, FRAMES) == TESSERA_PLUGIN_OK, "probe init");
    budget_plugin_call_t call = call_for(&n);
    for (unsigned i = 0; i < 3; ++i)
        require(plugin_call_block(&p, call.in_l, call.in_r, call.out_l, call.out_r, FRAMES) >= 0, "probe warmup");
    gic_enable_irq(TIMER_IRQ);
    uint64_t dt, start = clock_now();
    long r = budget_plugin_invoke(&call, interval, start + interval / 20u, &dt);
    require(r == BUDGET_DEADLINE && dt < interval && has_sound(&n), "absolute cutoff preemption");
    budget_t policy;
    budget_init(&policy, interval / 20u, 1);
    require(budget_plugin_run(&policy, &call, &dt) == BUDGET_KILL && !has_sound(&n), "probe cleanup");
    process_destroy(p.proc);
    uart_puts("absolute-cutoff: real spinning EL0 call preempted before its full CPU budget\r\n");
}

static void syscall_gate_probe(void)
{
    plugin_t p; node_t n = {0};
    require(plugin_load(&p, trampoline_svc_start,
            (size_t)(trampoline_svc_end - trampoline_svc_start), "gateattack") == PLUGIN_OK, "gate probe load");
    map_io(&n, &p);
    process_set_svc_gate(p.proc, PLUGIN_TRAMP_VA);
    process_set_liveness(p.proc, &n.liveness);
    require(plugin_call_init(&p, RING_SR, FRAMES) == TESSERA_PLUGIN_OK, "gate probe init");
    budget_plugin_call_t call = call_for(&n);
    budget_t b; budget_init(&b, interval / 10u, 3);
    uint64_t dt;
    int result = budget_plugin_run(&b, &call, &dt);
    require(result == BUDGET_FAULT && p.proc->state == PROC_KILLED &&
        n.liveness == PROC_LIVENESS_DEAD && !has_sound(&n), "trampoline SVC privilege isolation");
    process_destroy(p.proc);
    uart_puts("syscall-gate: forged contract SVC at a valid trampoline address rejected\r\n");
}

static void control_probe(void)
{
    plugin_t cp;
    require(plugin_load(&cp, temporal_ctl_start, (size_t)(temporal_ctl_end - temporal_ctl_start), "tctl") == PLUGIN_OK, "control load");
    uintptr_t page = phys_alloc_page_zero();
    require(page && !plugin_map_region(&cp, RESULTS_VA, page, PAGE_SIZE, VMM_READ | VMM_WRITE), "control mapping");
    volatile uint64_t *r = (volatile uint64_t *)P2V(page);
    r[0] = node[BEST].pid;
    r[1] = interval * (1ull << 32); /* must survive all six SVC argument registers */
    r[2] = interval;
    r[3] = interval - host.limits.job_overhead;
    r[4] = TC_BEST_EFFORT | ((uint64_t)TC_BYPASS << 8);
    tc_host_bind_control(&host);
    require(plugin_call_init(&cp, RING_SR, FRAMES) == TESSERA_PLUGIN_OK, "control invocation");
    require((long)r[16] == TC_ENODEV && (long)r[17] == TC_EINVAL &&
            (long)r[18] == TC_EINVAL && (long)r[19] == TC_EINVAL &&
            (long)r[20] == TC_OK && (long)r[21] == TC_EBUSY, "control return codes");
    int found = 0;
    for (unsigned i = 0; i < COUNT; ++i)
        if (host.scheduler.pending.task[i].pid == node[BEST].pid)
            found = host.scheduler.pending.task[i].period == r[1];
    require(found, "64-bit period preserved");
    process_destroy(cp.proc);
    tc_host_bind_control(NULL);
    uart_puts("contract-svc: 64-bit period, stale PID, invalid flags/ranges, pending-plan rejection PASS\r\n");
}

void test_main(void)
{
    uart_virt_init(); uart_puts("\r\n=== QEMU temporal contracts ===\r\n");
    pmm_init(); mmu_init(); exceptions_init(); gic_init();
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    interval = freq / HZ;
    gc_ring_ops_t ring_ops = {ring_new, ring_del, ring_map, ring_unmap, NULL};
    gc_init(&gc, &ring_ops); pm_init(&pm, &gc);
    pm_register_blob(&pm, "good", good_elf_start, (size_t)(good_elf_end - good_elf_start));
    pm_register_blob(&pm, "blip", blip_elf_start, (size_t)(blip_elf_end - blip_elf_start));
    pm_register_blob(&pm, "hog", hog_elf_start, (size_t)(hog_elf_end - hog_elf_start));
    size_t baseline = pmm_free_pages();
    cutoff_probe();
    syscall_gate_probe();
    require(pmm_free_pages() == baseline, "probe frame leak");
    tc_limits_t limits = {interval, interval / 100u, interval / 50u};
    tc_host_init(&host, &gc, &limits, clock_now);
    temporal_contract_t contracts[COUNT];
    for (unsigned i = 0; i < COUNT; ++i) {
        long pid = pm_load(&pm, i == GOOD ? "good" : i == BLIP ? "blip" : "hog");
        require(pid > 0, "plugin load");
        node[i].index = i;
        map_io(&node[i], pm_plugin(&pm, (uint32_t)pid));
        process_set_liveness(node[i].pl->proc, &node[i].liveness);
        require(plugin_call_init(node[i].pl, RING_SR, FRAMES) == TESSERA_PLUGIN_OK, "plugin init");
        tc_plugin_binding_t b = {call_for(&node[i]), node[i].in, node[i].in + FRAMES, publish, &node[i]};
        require(tc_host_bind(&host, &b) == TC_OK, "host binding");
        temporal_contract_t c = {0};
        c.pid = (uint32_t)pid;
        c.period = interval * (i == BLIP ? 2u : i == BEST ? 4u : 1u);
        c.deadline = i == GOOD ? interval / 3u : i == BLIP ? interval * 3u / 4u : interval * 9u / 10u;
        c.cpu_budget = i == GOOD ? interval / 10u : interval / 20u;
        c.criticality = i == GOOD ? TC_HARD : TC_SOFT;
        c.overrun_policy = policies[i];
        c.kill_after = i == STRIKES ? 3 : 0;
        c.skip_periods = i == DEGRADE ? 2 : 0;
        if (i == BEST) {
            c.deadline = interval; c.cpu_budget = interval - limits.job_overhead;
            c.criticality = TC_BEST_EFFORT;
        }
        contracts[COUNT - 1u - i] = c; /* registration order is deliberately backwards */
    }
    require(tc_host_stage(&host, contracts, COUNT) == TC_OK, "admission");
    aw_init(&worker, 1);
    require(tc_host_attach(&host, &worker) == TC_OK, "worker attach");
    spsc_init(&dac, dac_buf, 4096);
    audio_core_init(&audio, &dac, dma, FRAMES, interval / 2u);
    int16_t zero[SAMPLES] = {0};
    spsc_write(&dac, zero, SAMPLES); spsc_write(&dac, zero, SAMPLES);
    require(!smp_start_core(1, worker_entry, &worker, (uint64_t)(uintptr_t)(stack1 + sizeof stack1)), "CPU1 start");
    uint64_t start = clock_now();
    while (!worker.online && clock_now() - start < freq) { }
    require(worker.online, "CPU1 online");
    timer_init(HZ);
    __asm__ volatile("msr daifclr, #2");
    while (audio.serviced < BLOCKS) __asm__ volatile("wfi");
    timer_stop(); __asm__ volatile("msr daifset, #2");
    start = clock_now();
    while (!aw_drained(&worker) && clock_now() - start < freq) { }
    require(aw_drained(&worker), "worker drain");
    aw_stop(&worker);
    require(host.last_result == TC_OK && !worker.overruns && worker.blocks == BLOCKS &&
            !underruns && !audio.wd.overruns, "cadence continuity");
    require(published == COUNT, "publication order count");
    for (unsigned i = 0; i < COUNT; ++i) {
        const tc_task_state_t *v = tc_state(&host.scheduler, node[i].pid);
        require(v && !node[i].invalid && node[i].calls == BLOCKS, "full valid output every frame");
        require(publish_order[i] == node[i].pid, "criticality/EDF order, not registration order");
        require(v->deadline_misses == 0 && v->missed_releases == 0, "no admitted deadline miss");
        uart_printf("%s: runs=%u completed=%u budget=%u shed=%u killed=%u silent=%u dry=%u\r\n",
            names[i], (unsigned)v->runs, (unsigned)v->completed, (unsigned)v->budget_overruns,
            (unsigned)v->shed, v->killed, node[i].silent, node[i].dry);
        if (i == GOOD) require(v->completed == BLOCKS && !v->budget_overruns, "hard audio survives");
        if (i == BLIP) require(v->runs == 50 && v->completed == 48 && v->budget_overruns == 2 && node[i].dry == 52 && !v->killed, "independent period and bypass recovery");
        if (i == KILL || i == STRIKES) {
            require(v->killed && node[i].pl->proc->state == PROC_KILLED &&
                node[i].liveness == PROC_LIVENESS_DEAD && v->runs == (i == KILL ? 4u : 6u), "real termination policy");
            budget_plugin_call_t call = call_for(&node[i]);
            require(plugin_call_block(node[i].pl, call.in_l, call.in_r, call.out_l, call.out_r, FRAMES) == -1, "no killed-process reentry");
        }
        if (i == MUTE || i == BYPASS) require(v->runs == 100 && v->budget_overruns == 97 && !v->killed, "persistent mute/bypass policy");
        if (i == DEGRADE) require(v->runs == 36 && v->budget_overruns == 33 && v->shed == 64 && !v->killed, "bounded rate degradation");
        if (i == BEST) require(!v->runs && v->shed == 25 && !v->killed, "best effort cannot consume reserved time");
    }
    uart_printf("audio: callbacks=%u worker-skips=%u underruns=%u watchdog=%u\r\n",
        (unsigned)audio.serviced, (unsigned)worker.overruns, (unsigned)underruns, (unsigned)audio.wd.overruns);
    control_probe();
    __atomic_store_n(&host.scheduler.pending_ready, 0u, __ATOMIC_RELEASE); /* stopped/drained */
    require(tc_host_detach(&host) == TC_OK, "host detach");
    for (unsigned i = 0; i < COUNT; ++i) {
        require(tc_host_unbind(&host, node[i].pid) == TC_OK, "unbind before free");
        require(pm_unload(&pm, node[i].pid) == PM_OK, "unload");
    }
    require(pmm_free_pages() == baseline, "allocator baseline after all policies");
    uart_puts("TEMPORAL: PASS\r\n");
    m12_finish();
}
