---
id: chg-2026-06-22-ti4-work-conservation
type: chg
title: "TI-4: push placement, the busy-tick overload kick, and the 4 ms backstop"
date: 2026-06-22
arc: arc-tickless-idle
commits: ["542d1c62", "9d072b2a", "bd1c4012", "1956aacc"]
touched: [sub-kernel-sched-smp]
established: []
closed: []
opened: [seam-affinity-mask, seam-tickless-bare-metal]
mirrors-checked: []
depth: rich
---
## What

Restored the work-conservation that NO_HZ_IDLE had silently removed:

- **TI-4b push placement** — in production, prefer an *idle* peer for a
  waking thread (rotating across distinct idle CPUs) so a burst from one
  busy producer spreads instead of piling on the waker's tree behind a
  single best-effort kick. Plus cpu0 production-gating: the test phase
  stays periodic, because there the 1 kHz re-poll is load-bearing for
  cross-CPU handoffs that have no wake IPI.
- **TI-4c the busy-tick kick** — a *still-ticking* busy CPU with surplus
  queued work and a parked peer kicks one peer to come steal.
  [[spec-sched-rebalance]] is its model, with `buggy_nokick` and
  `buggy_nolift` separating "the mechanism is absent" from "the mechanism
  fires and the wake is still lost".
- **TI-4d telemetry** — the work-conservation counters: a park committed
  while work is queued elsewhere charges its whole duration as *starved*,
  split total vs tickless, plus a wake-source split.
- **TI-4e** — the backstop 100 ms → 4 ms, and the affinity-ready seam
  ([[seam-affinity-mask]]).

## The finding, which is the reason the chunk exists

**The residual tickless boot slowdown is not a guest bug.** The wake path
is push-complete — the telemetry measured **99.85% of tickless parks
IPI-woken**, not backstop-woken — but resuming a *deep-parked* vCPU via
SGI costs ~0.85 ms under HVF against ~7 µs hot: GICv2-MMIO vmexits plus
the host vCPU-thread resume. An emulation artifact.

So the 4 ms re-poll is honestly an **HVF dev-loop warmth knob, not a
scheduler fix** (HVF boot ~7 s vs ~17–35 s at 100 ms, at ~5% idle vs
0.3%). On bare metal an SGI to a WFI'd core is nanoseconds, so deep-park
already gives both fast boot and ~0% idle there — which is
[[seam-tickless-bare-metal]], owed a confirmation.

The kick earns its place independently: the fast (~1 ms) parallel-surplus
catch, cpubench starvation −34% to −60% across parallel modes.

## Why the small mechanism, not a full idle-side balancer

The committed scripture is already the Linux NO_HZ_IDLE push model
(push-placement + push-on-overload + backstop). The kick was the one
piece specced at TI-4a and never wired. The dwell/distance/state-machine
alternatives were disproven in-session — they chased per-park write cost,
which is not what the measurement said was expensive.

## Audit ([[adt-ti4-r1]])

Opus-4.8-max prosecutor plus a concurrent self-audit, **0/0/0/2** — both
P3s pre-existing test fragility shared with sibling `sched.*` tests
([[fnd-ti4-r1-f1]], [[fnd-ti4-r1-f2]]). The SOUND set is unusually
worth keeping: the kick takes no locks and is re-entrant in IRQ context;
the surplus read is provably benign (relaxed, never a deref, stale means
a spurious or skipped kick that self-corrects next tick); the affinity
predicate is provably inert; and the test phase is byte-identical to the
pre-chunk periodic idle because both gates flip after `test_run_all`.
