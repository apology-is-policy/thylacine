---
id: adt-ti4-r1
type: adt
title: "TI-4e: the work-conservation kick, the affinity seam, and the 4 ms backstop"
date: 2026-06-22
scope: [sub-kernel-sched-smp]
reviewer: opus
model-start: "claude-opus-4-8 (max)"
model-end: "claude-opus-4-8 (max)"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-ti4-r1-f1, fnd-ti4-r1-f2]
round-of: chg-2026-06-22-ti4-work-conservation
created: 2026-08-01
---
## Scope

The busy-tick push-on-overload kick, the inert affinity-ready predicate,
the tickless backstop retune, and the wake-source telemetry.
Opus-4.8-max prosecutor (MODEL start == end, no Fable fallback) plus a
concurrent self-audit; the two passes converged on the sound set.

## Verdict

**0 P0 / 0 P1 / 0 P2 / 2 P3.** Not dirty. Both P3s are pre-existing test
fragility shared with sibling `sched.*` tests, not introduced here, and
neither was fixed: [[fnd-ti4-r1-f1]], [[fnd-ti4-r1-f2]].

## Verified sound — do not re-prosecute without a new vector

- **The kick takes no locks.** `sched_tick -> sched_notify_idle_peer ->
  gic_send_ipi` is one volatile MMIO store plus a barrier (or one system
  register write), re-entrant in timer-IRQ context, on a path the
  existing `ready_on`/`wakeup` already use.
- **The lock-free surplus read is benign.** Relaxed loads of 8-byte
  aligned band-head pointers, never a Thread deref; a race against a
  peer's unlink yields old-or-new-or-NULL, never torn. A stale boolean is
  a spurious or skipped kick that self-corrects on the next tick.
- **No thundering herd, no self-IPI, no livelock**: at most one IPI per
  tick per busy CPU, GIC-coalesced, no-op-handled, self-terminating once
  the peer wakes and steals.
- **`thread_may_run_on` is provably inert**: `return true;` collapses
  `select_target_cpu`'s two guards to their originals and makes
  `try_steal`'s condition byte-identical.
- **The 4 ms backstop is bound-safe**: 250,000 counter ticks at 62.5 MHz,
  inside the timer's reload clamp; `tickless_target_cnt` only ever
  shrinks the backstop, so it introduces no overflow. Measured max
  starved park: 5 ms — the backstop demonstrably bounds it.
- **The wake-source classify cannot perturb [[inv-i9]]**: it runs after
  the flag clear and before the timer restore, is one counter read plus
  two relaxed increments, and touches no timer, run-tree, rendez or flag
  state. The wake has already happened.
- **Test-phase inertness**: both gates flip after `test_run_all`, so
  during tests the chunk is byte-identical to the pre-tickless periodic
  idle.

## The finding that is not a finding

The audit's substantive output was not a defect but a **measurement**:
the tickless boot slowdown is HVF deep-park vCPU resume latency, not a
guest bug — 99.85% of tickless parks are IPI-woken, so the wake path is
push-complete, and what remains is ~0.85 ms of emulator resume cost
against ~7 µs hot. Recorded on the chunk and owed a bare-metal
confirmation at [[seam-tickless-bare-metal]].
