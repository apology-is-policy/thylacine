---
id: spec-net-poll
type: spec
title: "net_poll.tla"
models: [sub-kernel-ninep-dev9p-poll, sub-netd-server, sub-ptyfs]
pins: [inv-i9]
cfgs:
  - "net_poll.cfg -- clean, timed (timeout 0 included): Invariants + FailSafeSilent (118 states)"
  - "net_poll_notimeout.cfg -- clean, poll(-1): the same (36)"
  - "net_poll_liveness.cfg -- Spec_Live, poll(-1): PollerEventuallyServed (36)"
  - "net_poll_liveness_timeout.cfg -- Spec_Live, timed: PollTerminates + PollerEventuallyServed (118)"
  - "net_poll_hung.cfg -- HUNG_SERVER, Spec_Live, timed: Invariants + PollTerminates (308)"
  - "net_poll_failsafe_fires.cfg -- HUNG_SERVER: FailSafeSilent VIOLATED by design -- the fail-safe is reachable"
  - "net_poll_buggy_cache_only_sample.cfg -- the design before #98: a zero-timeout poll of a ready socket reads an empty cache (NoFalseNotReady counterexample)"
  - "net_poll_buggy_stale_cache.cfg -- the same design: an answered arm's cache outlives a retract (NoFalseReady counterexample)"
  - "net_poll_buggy_settle_cut_by_deadline.cfg -- a settle bounded by the call's deadline guesses about a healthy server (NoFalseNotReady counterexample)"
  - "net_poll_buggy_gc_snapshot.cfg -- the stranded-op collector flushes a snapshot (NoFalseNotReady counterexample)"
  - "net_poll_buggy_lost_ready.cfg -- a park with no arm ensured (NoMissedNetPoll counterexample)"
  - "net_poll_buggy_edge_arm.cfg -- the server answers an arm only for a later rise (PollerEventuallyServed counterexample)"
gate: "specs/check-net-poll.sh for ANY change to the dev9p readiness protocol -- the snapshot, the settle and its fail-safe, the arm, the relay, the stranded-op collector, or the server's `ready` file (spec-first re-enabled for this surface)"
created: 2026-07-31
updated: 2026-09-28
---
## Abstraction

One poller, one socket (or pty), its server, and the per-client poll-pump
kthread's relay. Readiness is a LEVEL for the requested mask: it rises and it
falls. The design is the SAMPLE/ARM split
([[dec-2026-09-28-poll-sample-arm-split]]). The SNAPSHOT readiness read,
answered at once, is the only sample. The ARM, held by the server until the
file is ready and evaluated when it arrives, is sent only before a park, and
its answer is only a wake. Every pass scans, settles, then decides. A snapshot
still unanswered 1 s after it was sent is flushed, reported not ready and
counted; `HUNG_SERVER` makes that reachable.

The server is the `ready` file of [[sub-netd-server]] and [[sub-ptyfs]]. The
N-fd loop, the local fds beside a socket, death, stops and the snapshot's
lifetime are [[spec-poll]]'s; the arm's teardown and the Tclunk that frees the
server's slot are [[spec-net-poll-teardown]]'s. Deliberately beneath the
model: the multi-client pump fairness, the widening of one arm's mask across
pollers, OOM degrades, and the 9P client's discard of a flushed op's late
reply (tag uniqueness, [[spec-9p-client]]).

## What it pins

- **NoFalseNotReady / NoFalseReady** -- a verdict is the server's own answer,
  given inside the pass that reports it; a pass ghost carries the instant. The
  one excused guess is the counted fail-safe against a server that had stopped
  answering.
- **ArmBeforePark / NoMissedNetPoll** -- [[inv-i9]] across the relay: a parked
  poller is hooked and has a wake coming (an arm outstanding, its answer in
  the relay, or its flag already set).
- **PollerEventuallyServed / PollTerminates** -- a socket that becomes ready
  and stays ready returns the poll; a timed poll returns even against a hung
  server.
- **FailSafeSilent** -- holds against a server that answers, and its violation
  under `HUNG_SERVER` is the positive control. That a healthy server answers
  within 1 s is a timing assumption the model states and cannot check; the
  runtime owns it (the counter, the gates, a test server that defers).

Before #98 the module modeled the old bridge: one deferred read that was both
the sample and the wake, read back through a cache, over a monotonic `ready`.
It could not see #98. A monotonic level has no stale cache, and a poller that
only parks never decides from a cache. That design survives as the red
`BUGGY_CACHE_ONLY_SAMPLE`.

## Action-site map

Filled at NP-4, when the split lands in `kernel/dev9p_poll.c` and
`kernel/poll.c`; the `ready` file's snapshot branch lands at NP-3. Until then
the code is `BUGGY_CACHE_ONLY_SAMPLE`. The table lives in
`specs/SPEC-TO-CODE.md::net_poll.tla`.
