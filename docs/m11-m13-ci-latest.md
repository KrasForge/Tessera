# Latest M11/M13 CI outcome — 2026-09-16

Commit `f2d9bfe08bf9a0fa260270425d11bb444d571330`, run **35144976196**:
**failure**. M11 capacity, cross-core PCM and faults on CPUs1/2/3 passed.
The first serial boot completed 18 live graph edits with zero output-frame
misses, watchdog overruns, and plugin budget/deadline failures.
The second independent boot restored the session and exact PCM hash, rejected
bad/missing/partial session files while retaining the old graph, and applied
the live gain change correctly. The final strict timing assertion failed:
source 104 completions, one deadline miss / one shed job; gain 105 completions,
no deadline/budget failures. Consequently the full hosted gate is NOT green.

Full local functional acceptance had passed; this does not erase the hosted
failure. The root cause of every wall-clock miss has not been conclusively
isolated. The fast 48 kHz / 64-sample diagnostic and physical CM4/I2S acceptance
are not established. See `m11-m13-verification.md` and
`m11-m13-workstation.md` for the explicit test profiles and implementation.

This outcome record was saved locally after the final source publication. No
further code push, merge, issue closure, or background work was scheduled.
