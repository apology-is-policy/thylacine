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
5 closed findings on [[sub-kernel-sched-smp]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-33-r1-f1]] [P2] #363: a CPU parked up to the tickless backstop over its own just-requeued thread (fixed)
- [[fnd-33-r2-f1]] [P3] The park-guard comment misattributed the no-lost-wake guarantee to a flag-gated IPI (fixed)
- [[fnd-866-r1-f2]] [P3] try_steal scanned only the band head, so a thread queued behind a pinned one was unstealable (fixed)
- [[fnd-ti4-r1-f1]] [P3] The surplus test's baseline assertion assumes no NORMAL kthread is queued at test entry (documented) — Documented, not fixed. It is **shared with every sibling `sched.*`
- [[fnd-ti4-r1-f2]] [P3] The ready() -> read-head window in the surplus test is preemptible (documented) — Documented, not fixed. The pattern is identical to the established one in
<!-- generated:end -->
