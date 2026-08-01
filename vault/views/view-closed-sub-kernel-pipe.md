---
id: view-closed-sub-kernel-pipe
type: view
title: "Do-not-re-report preamble — sub-kernel-pipe"
query: closed:sub-kernel-pipe
---
# Do-not-re-report preamble — sub-kernel-pipe

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-pipe`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

One standing fact: the wait/wake protocol's coverage is
[[spec-pipe]]'s four buggy configs (one per wake site) — a green run
of the family IS coverage of the wake set, unlike the scheduler
family's named blind spots. What the model cannot see is lifetime:
the ring refcount ([[fnd-r15b-f234]]) and the poll list are below
its abstraction.

<!-- generated:begin -->
1 closed findings on [[sub-kernel-pipe]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-r15b-f234]] [P2] pipe_ring.ref was a plain -- : concurrent endpoint closes could double-free the ring (fixed)
<!-- generated:end -->
