# Graph scheduling across cores (M11)

How Tessera distributes the audio graph over the secondary cores: the
execution model for edges that cross cores, the placement heuristic, and the
glitch-free way a new node-to-core plan replaces a running one.  Implemented
by [`arch/arm64/graph_sched.c`](../arch/arm64/graph_sched.c) (issue #75) on
top of the per-core audio workers of issue #74
([`arch/arm64/audio_worker.c`](../arch/arm64/audio_worker.c)).

## The model: same-block on a core, pipelined across cores

Every block, CPU0 (the audio core) kicks each worker once; each worker runs
its assigned nodes and answers.  Workers never wait on CPU0, on each other,
or on locks - that is the issue #74 contract, and partitioning must not
weaken it.  So:

- **Within one worker, nodes run in topological order.**  A same-core edge
  has *same-block* semantics: the consumer reads what its producer wrote in
  the current block.  A chain scheduled on one core behaves exactly as it did
  single-core.
- **An edge between cores is pipelined.**  The consumer reads what the
  producer wrote *last* block: one block of added latency per cross-core hop,
  and in exchange no worker ever waits for another mid-block.  When an edge
  becomes cross-core, its ring is reset and primed with one block of silence
  so the pipeline starts in steady state.
- **Edges into the DAC are always pipelined.**  CPU0 drains its ring
  immediately after kicking the workers, so it necessarily reads the previous
  block's output.  This was already true before partitioning.

The alternative - same-block semantics across cores, with workers spinning on
upstream completion - was rejected: it reintroduces cross-core waiting on the
audio path, makes a stalled plugin stall other cores (defeating issue #74's
containment), and its latency win disappears as soon as the graph is deeper
than the block budget anyway.

**Latency accounting:** a chain on one core reaches the DAC in 1 block (the
DAC pipeline).  Each cross-core hop on the path adds exactly +1 block.  At
the M4 numbers (1 kHz blocks) that is +1 ms per hop, spent buying full
parallelism.

## Placement: balance-first, chains follow their producer

`graph_plan_compute()` walks the topological order (Kahn, issue #27) and
assigns each plugin node a core:

1. Prefer the core of the node's first upstream plugin producer - a chain
   stays on one core, keeping same-block latency - **until** that core holds
   its fair share `ceil(n_plugins / n_workers)` of nodes;
2. otherwise take the least-loaded core (lowest index on ties).

The result is deterministic: the same graph always yields the same plan.
With one worker every plan degenerates to the single-core schedule.  The
DAC is always "placed" on CPU0.  Balance-first is deliberately simple - the
planner cannot yet know what a node *costs*; cost-aware packing (pack chains
up to a measured cycle budget instead of a node count) arrives with the
per-plugin time accounting of issue #77.

## Swapping plans without a glitch

Plans are computed by the control plane (never on the audio path) and handed
to CPU0 through a seqlocked staging slot:

- **Stage** (control plane, single writer): after every graph mutation -
  [`graph_control`](../arch/arm64/graph_control.c) fires an `on_change` hook
  on load/unload/wire/unwire - the control plane recomputes and overwrites
  the slot, bumping the stage generation odd/even around the write.  Staging
  never blocks, and a newer stage simply replaces an unconsumed older one:
  only the newest plan ever reaches the audio core.
- **Apply** (CPU0, at block start, before kicking): if the stage generation
  moved and **every worker has drained** (answered every kick), CPU0 copies
  the slot, re-checks the generation (a torn copy just retries next block),
  then commits: it resets/primes exactly the edges whose placement changed,
  rewrites the worker node tables in per-worker topological order, and bumps
  the applied generation.  Total work is bounded (16 nodes, 32 edges) and
  runs well inside one block budget.  If any worker is still busy, nothing
  happens and CPU0 retries next block - the running plan stays valid, audio
  keeps flowing, CPU0 never waits.

Edge transitions on apply:

| transition                      | action                       |
|---------------------------------|------------------------------|
| new edge, or ring swapped by rewire, cross-core | reset + prime one silence block |
| new edge, or ring swapped, same-core            | reset (empty; same-block needs no prime) |
| same-core -> cross-core         | reset + prime one silence block |
| cross-core -> same-core         | reset (drop the in-flight block, else it becomes permanent latency) |
| placement unchanged             | untouched - keeps streaming  |

A repartition is therefore *click-bounded*, not seamless: each re-placed edge
carries one block of silence (or drops one in-flight block) at the switch.
Unchanged edges are never touched.  CPU0's cadence is never disturbed - the
QEMU acceptance harness rewires and splits a live chain mid-run and requires
zero watchdog overruns throughout (`make test-arm-gsched-qemu`).

A worker stalled inside a plugin defers reconfiguration indefinitely (its
kicks are skipped and charged, per issue #74) - audio on the other cores is
unaffected, and the per-plugin CPU budgets of issue #78 will kill such a
plugin.

## Observability

The active assignment renders as one stats line (`graph_sched_format`),
printed over UART alongside the `audio_latency:` line:

```
graph_sched: gen=3 workers=2 cross=1 n0(pid=1)->w0 n1(pid=2)->w1 dac->cpu0
```

`gen` counts applied plans, `workers` is the scheduling width the plan used,
`cross` counts plugin-to-plugin edges that pay the +1 block pipeline hop, and
each node shows its core.  `wN` is the worker on CPU N+1.

## Tests

- `make test-arm-gsched` - host unit tests (ASan/UBSan): the planner
  (chains, capacity splits, diamonds, cycle rejection, determinism), the
  stage/apply seqlock (busy-worker deferral, torn-stage retry, newest-stage
  wins, poisoned-stage consumption), the edge reset/prime table above, the
  `graph_control` hook, and the stats line.
- `make test-arm-gsched-qemu` - the four-core acceptance run: a live
  synth -> filter -> DAC chain is rewired at runtime and then split across
  two cores mid-run; the DAC stream must stay a consecutive function of the
  synth's block counter across both transitions (bit-identical steady state,
  shifted by the pipeline latency), CPU0 must service every callback with
  zero overruns, and the never-scheduled third worker must stay parked.

## Feedback edges (one-block delay, issue #117)

The graph is a strict DAG for same-block scheduling, but feedback-delay and
reverb topologies need a cycle.  A **feedback edge** closes that cycle without
breaking the schedule: it carries the producer's *previous* block to its
consumer (an explicit one-block delay), so it imposes no same-block ordering.

- `audio_graph_connect_feedback(g, src, dst)` (control plane:
  `gc_connect_feedback`) marks the edge feedback.
- `audio_graph_toposort` excludes feedback edges from the in-degree, so a graph
  whose only cycle is closed by feedback edges still sorts - while a cycle closed
  by a **normal** edge is still rejected (`-1`).
- The multi-core planner (`graph_sched.c`) treats a feedback edge as delayed:
  it does not pin the consumer to the producer's core and is not counted as a
  same-block cross-core barrier.

Covered by `make test-arm-graph` (the feedback-edge cases) and the existing
`make test-arm-gsched` planner tests.

## Mixer / routing primitives (issue #118)

Real signal chains need summing, level and pan, a wet/dry blend, and a true
bypass. `arch/arm64/mixer.c` provides these as Q15 fixed-point operations on the
int16 PCM blocks, so the mixer, send/return, and gain/pan graph nodes run on the
`-mgeneral-regs-only` audio path:

- **`mix_gain` / `mix_add`** - apply a gain, or accumulate a source into a bus,
  saturating on overflow (summing several inputs into one mix bus).
- **`mix_pan`** - mono to a stereo pair with a linear (constant-gain) law
  (`0` = hard left, `MIX_ONE` = hard right).
- **`mix_blend`** - wet/dry blend, the same unity-sum mix as the crossfade
  (#103), so a `dry == wet` blend is transparent.
- **`mix_bypass`** - true bypass, the dry signal bit-for-bit (pairs with a
  hardware relay for analog bypass).

Covered by `make test-arm-mixer`.

## Multi-channel buses (issue #119)

Edges are stereo by default, but a node can declare its own input and output
widths so a graph can carry mono sends, stereo, or wider surround/ambisonic
buses. Each node has an `in_ch`/`out_ch` count (both default to 2), settable
with `audio_graph_set_channels(g, node, in_ch, out_ch)` (a count of `0` leaves
that side unchanged). A connect requires the producer's `out_ch` to equal the
consumer's `in_ch`; a mismatch is rejected and leaves the edge table untouched,
so a stereo plugin can only wire to a stereo input and a 6-channel source only
to a 6-channel consumer. The accepted edge records the width it carries, read
back with `audio_graph_edge_channels(g, e)`, which the kernel uses to size the
ring buffer that backs the edge. Because the defaults are 2/2, every existing
stereo graph connects exactly as before.

Covered by `make test-arm-graph`.
