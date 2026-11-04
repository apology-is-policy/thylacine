---
id: view-closed-sub-kernel-sched-smp
type: view
title: "Do-not-re-report preamble — sub-kernel-sched-smp"
query: closed:sub-kernel-sched-smp
---
# Do-not-re-report preamble — sub-kernel-sched-smp

Generated from `fnd-*` notes (`quaestor render`; also emitted on-demand
by `quaestor closed sub-kernel-sched-smp`). Paste or transclude into a
prosecutor prompt as the closed-findings preamble.

Read it WITH the verified-sound sets in [[adt-ti4-r1]] and
[[adt-363-r2]], and with two standing facts about this surface:

- **The spec family has a named blind spot.** [[spec-sched-tickless]]
  cannot express #363 (it has no self-requeue action), so a green run of
  the whole family is not coverage of the park-commit logic. See
  [[fnd-33-r1-f1]].
- **Two findings here were comments, not code** ([[fnd-33-r1-f2]],
  [[fnd-33-r2-f1]]), and both were worth their severity because of what
  they would have licensed — in one case an "optimization" that
  reintroduced the exact bug the chunk had just closed. Prose that
  understates a mechanism reads as permission to relax it.

<!-- generated:begin -->
9 closed findings on [[sub-kernel-sched-smp]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-33-r1-f1]] [P2] #363: a CPU parked up to the tickless backstop over its own just-requeued thread (fixed)
- [[fnd-33-r2-f1]] [P3] The park-guard comment misattributed the no-lost-wake guarantee to a flag-gated IPI (fixed)
- [[fnd-866-r1-f2]] [P3] try_steal scanned only the band head, so a thread queued behind a pinned one was unstealable (fixed)
- [[fnd-c8a-r2-f2]] [P2] scheduler.pinned_wake_goes_home could not reach its own assert when the mechanism broke (fixed) — The waker calls `sched()` while it waits, so a misplaced boot thread is dispatched and the test reads where it is.
- [[fnd-c8a-r2-f3]] [P3] The runner's off-CPU check could redden the wrong test (fixed) — An `OFF-CPU0` marker prints either way, and only the first test to return off CPU 0 answers for it.
- [[fnd-c8a-r2-f4]] [P3] A pinned thread's home is a dispatch stamp with no guard (fixed) — `ready_on` extincts on a pinned thread that has run being placed off its `last_cpu`, and the comments the fix made false are corrected.
- [[fnd-c8a-r2-f5]] [P3] In the test phase the fix strands a pinned sleeper homed on a parked secondary (fixed) — The IPI is not gated for a pinned thread, for which it is correctness and not promptness.
- [[fnd-ti4-r1-f1]] [P3] The surplus test's baseline assertion assumes no NORMAL kthread is queued at test entry (documented) — Documented, not fixed. It is **shared with every sibling `sched.*`
- [[fnd-ti4-r1-f2]] [P3] The ready() -> read-head window in the surplus test is preemptible (documented) — Documented, not fixed. The pattern is identical to the established one in
<!-- generated:end -->
