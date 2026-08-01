---
id: view-closed-sub-pouch-seam
type: view
title: "Do-not-re-report preamble — sub-pouch-seam"
query: closed:sub-pouch-seam
---
# Do-not-re-report preamble — sub-pouch-seam

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-seam`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH one standing fact: the recurring defect on this surface is
not in the seam's logic but in its **drift gate**. The build's
seam-check list has been found un-extended in two separate rounds
([[fnd-threads9b-r1-f5]], [[fnd-signals13b-r1-f1]]) — same bug, same
codebase, one round apart, with the first already in the closed list. A
prosecutor finding it a third time has found a process failure, not a new
bug: the obligation belongs to any patch that adds a number.

The other standing fact is [[fnd-seam-r1-f1]]'s shape — a guard on the
path you are reading is not a guard on the mechanism. Two syscall paths
exist.

<!-- generated:begin -->
4 closed findings on [[sub-pouch-seam]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-seam-r1-f1]] [P0] The cancellable syscall path had no sentinel guard — a retargeted cancellation point issued svc with x8=0xFFFF (fixed)
- [[fnd-seam-r1-f6]] [P3] `patch -t` silently skips an already-applied patch (fixed)
- [[fnd-signals13b-r1-f1]] [P1] The seam-check list was not extended for the five note syscall numbers (the threads-round F5, verbatim) (fixed)
- [[fnd-threads9b-r1-f5]] [P2] The build's seam-check list was not extended for the round's four new syscall numbers (fixed)
<!-- generated:end -->
