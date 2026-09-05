---
id: view-closed-sub-pouch-thread
type: view
title: "Do-not-re-report preamble — sub-pouch-thread"
query: closed:sub-pouch-thread
---
# Do-not-re-report preamble — sub-pouch-thread

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-thread`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH the tid-publication history: the layer now carries TWO
independent guarantees (the kernel's `CLONE_PARENT_SETTID`-equivalent
publish at #112, and the child's own self-set at #111), and the second is
RETAINED deliberately even though the first subsumes it. A finding that
one is redundant is a finding about a defense that was added because the
race only appears at `-smp>1`.

The documented-not-fixed hazard ([[fnd-threads9b-r1-f2]], absent stack
guard pages) is a seam, not an open bug; finding it again is finding
[[seam-pouch-guard-pages]].

<!-- generated:begin -->
3 closed findings on [[sub-pouch-thread]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-threads9b-r1-f1]] [P1] pthread_cond_timedwait with a >1h timeout spins at 100% CPU (fixed)
- [[fnd-threads9b-r1-f2]] [P1] pthread stack guard pages are silently disabled (documented) — Documented, not fixed — the real fix needs a kernel syscall that can flip
- [[fnd-threads9b-r1-f5]] [P2] The build's seam-check list was not extended for the round's four new syscall numbers (fixed)
<!-- generated:end -->
