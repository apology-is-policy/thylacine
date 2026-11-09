---
id: view-closed-sub-kernel-torpor
type: view
title: "Do-not-re-report preamble — sub-kernel-torpor"
query: closed:sub-kernel-torpor
---
# Do-not-re-report preamble — sub-kernel-torpor

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-torpor`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

Read it WITH the surface's standing shape: the [[inv-i9]] proof here
is PROSE (no futex.tla — the suspension's first worked example), so
the do-not-re-report list carries more weight than usual — the
audit rounds ARE the formal record. The documented-not-fixed hazard
([[fnd-torpor8-r1-f2]], the lock-across-wakeup spin) is a seam, not
an open bug; finding it again is finding
[[seam-torpor-lock-wake-spin]].

<!-- generated:begin -->
10 closed findings on [[sub-kernel-torpor]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-c8a-r1-f1]] [P1] A peer's MAP_FIXED between the wait's pre-fault and its locked load sleeps under the bucket's lock (fixed) — The FILE miss refuses a faulter holding a counted lock, before the pin, and the wait reads the failed load as a spurious return. Also found by self-audit, at 4494357a1.
- [[fnd-c8a-r1-f2]] [P2] torpor.record_outlives_the_wake passed as a skip on one CPU (fixed) — It says so through `test_requires`.
- [[fnd-c8a-r1-f3]] [P2] The same test held a negative a never-run waiter satisfies, and a 200 ms race (fixed) — The waker is placed on a peer and kicked, with a far deadline and the cascade's wake as the second source, and the spin count is its positive control.
- [[fnd-c8a-r1-f4]] [P3] Readers of torpor_lock, and the ARCH order line skipped g_timerwait.lock (fixed) — Corrected.
- [[fnd-c8a-r1-f5]] [P3] torpor.wake_counts_only_a_sleeper leaked on a failure (fixed) — Every step is bounded and recorded, with the asserts after the release.
- [[fnd-c8a-r2-f1]] [P2] The model is green on the lost wake K06 produces: a mark between a waiter's read and its registering (fixed) — `SleepRead` and `SleepCommit` are the two ends of the rendez lock's hold, and `Reeval` folded into them. `MarkedSleeperIsHeld` is new and `NoLostWake` is tightened. The buggy cfg fails on the new invariant, and every other cfg still fails on its own.
- [[fnd-c8a-r2-f6]] [P3] The cond's stall wrote a shared line on every wait, and an armed stall could leak (fixed) — A load first, arming once both threads exist, and the runner releases a stall left armed and reddens the test that left it.
- [[fnd-c8a-r2-f7]] [P3] A test's comment claimed more than its fixture gives (fixed) — It says so.
- [[fnd-torpor8-r1-f1]] [P2] WAKE counted matched-and-marked waiters, not actual wakeups (fixed)
- [[fnd-torpor8-r1-f2]] [P2] torpor_lock held across wakeup()'s on_cpu spin — global futex serialization (documented) — Documented, deliberately: dormant at v1.0 (single-Proc-mostly), the
<!-- generated:end -->
