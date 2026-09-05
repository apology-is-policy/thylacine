---
id: inv-i17
type: inv
title: "I-17 — the EEVDF quantitative latency bound (a design target)"
number: I-17
guards: [sub-kernel-sched, sub-kernel-timer]
validated-by: [spec-scheduler, gate-smp]
strength: prose
created: 2026-08-01
updated: 2026-08-02
---
## Statement

A runnable thread's dispatch latency is **bounded** — not merely finite.
The intended form is EEVDF's: with virtual deadline
`vd_t = ve_t + slice × W_total / w_self`, a thread's wait is bounded by a
function of the runnable weight it is competing against, which is what
turns the VISION §4.5 p99/p99.9 latency cells into something provable
rather than measured.

## Enforcement

**It is not enforced.** This entry exists to record that honestly, because
the surrounding vocabulary (bands, `vd_t`, "EEVDF") implies otherwise.

What is built is a monotonic yield counter: on yield, `vd_t` is stamped
past every currently-queued thread, so a band rotates FIFO. There is no
weight in the calculation — `Thread.weight` exists, defaults to 1, and is
read by nothing. The as-built guarantee is therefore [[inv-i8]]'s
qualitative one plus a 6 ms slice; the *bound* is a target.

Three things do real work toward the target and are worth not confusing
with it:

- **Wake-preemption** (RW-11 SA-1b) closes the empirically-pinned 6 ms
  "slice cliff": before it, a newly-runnable higher-band thread waited up
  to a full slice for the next tick-driven preempt, because only the
  cross-CPU branch requested a reschedule. Now a same-CPU wake that
  outranks the running thread sets `need_resched`, and the
  syscall-return preempt point consumes it as the waker returns to EL0.
- **The `vd_t` clamp on cross-CPU wake** (RW-2 2A-F1) stops a stale key
  minted by a foreign CPU's clock from tailing a thread behind every
  fresh yielder — a starvation bounded only by the inter-CPU counter
  gap, which is an I-17 violation in the strict sense.
- **The cross-CPU `need_resched`** ([[fnd-866-r1-f1]]) closes the same
  leak on the placement path.

Each of those removes a specific unbounded-looking wait. None of them
establishes a bound.

## Validation

[[spec-scheduler]] checks `LatencyBound` as a temporal property, but the
property it checks is the *qualitative shadow* — eventually-runs under
fairness — not a quantitative bound; the per-thread fairness refinement
(a `Yield(cpu, t)` parameterized action) is explicitly deferred alongside
the EEVDF math. [[gate-smp]] proves nothing about latency at all; the
empirical evidence is the `irq-bench` / `cpubench` measurements and the
work-conservation telemetry, which are observations, not guarantees.

**blind-to:** everything quantitative. Treat any claim that Thylacine
"has EEVDF" as describing the intended design. The gap is
[[seam-eevdf-math]], and it is the reason [[inv-i17]] is the one
scheduler invariant carried at `prose` strength.
