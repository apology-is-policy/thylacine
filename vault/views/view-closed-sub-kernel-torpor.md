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
2 closed findings on [[sub-kernel-torpor]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-torpor8-r1-f1]] [P2] WAKE counted matched-and-marked waiters, not actual wakeups (fixed)
- [[fnd-torpor8-r1-f2]] [P2] torpor_lock held across wakeup()'s on_cpu spin — global futex serialization (documented) — Documented, deliberately: dormant at v1.0 (single-Proc-mostly), the
<!-- generated:end -->
