---
id: adt-b1c-r1
type: adt
title: "B-1c (the native heap) round 1: an endless line read as grep's \"no match\", a reader that left failed an innocent pipeline, and a witness leg that could not fail"
date: 2026-09-24
scope: [sub-thyla-heap, sub-libthyla-rs, sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters, sub-manual]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 10}
findings: [fnd-b1c-r1-f1, fnd-b1c-r1-f2, fnd-b1c-r1-f4, fnd-b1c-r1-sa4]
round-of: chg-2026-09-24-b1c-round1-close
created: 2026-09-24
---
## Scope

Branch `b1c-native-heap` at f7da956e (WIP 3): thyla-heap (dlmalloc 0.2.14 over
reservations the platform owns, the direct path at 256 KiB, the footprint and
peak accounting) with its ten host tests, libthyla-rs's `ThylaAlloc` backend and
`slurp`, `coreutils::stream` and the six filters rewritten on it, the six former
`ThylaAllocN` programs, the manual's bounds harness, and `/heap-probe`. Fable
died of credits at spawn, so the round ran on Opus 5.5 at max effort, told that
context independence was what it brought and to re-derive every claim the
design rests on from the code.

## Convergence

0 P0 / 1 P1 / 1 P2 / 10 P3 from the round, merged with the main session's
parallel self-audit (S-A1..S-A4; S-A1 is F4 and S-A3 is F12). Dirty by the shape
of the fixes -- a line bound through every streaming consumer, a gone-reader
policy across nineteen filters, the alignment routing -- so a round 2 on the
fixes follows. F1 ([[fnd-b1c-r1-f1]]) is the finding of the round: the streaming
that closed HT09.R4-F2 removed the slurp's cap without a bound of its own, so an
endless line grew until the pool ran out and the fault kill read as grep's "no
match". F2 ([[fnd-b1c-r1-f2]]) and the self-audit's S-A4 ([[fnd-b1c-r1-sa4]]) are
the two halves of one policy, #54: a reader that left was an error, and the
streaming filters kept reading after it left. F4 ([[fnd-b1c-r1-f4]]), rated P2 by
the self-audit, was a witness leg that could not fail. The P3s: unbounded
consumers left without the old 4 MiB containment and comments claiming the fault
ends the program that grew (F3); no device leg for a live block across a trim or
for an automatic reservation release (F5); a records vector dropped before the
census (F6); behaviour changes unrecorded (F7); the footprint bounding the data
pages rather than the kernel's whole count (F8); an alignment past a page mapping
a block on its own (F9); stale "fixed heap" comments (F10); a long line's room
kept to exit (F11); `add_direct`'s overflow (F12); and the self-audit's `Tail` of
zero lines holding its input (S-A2). All fixed before landing except what is owed
to the operator: F3's consumers (victim selection, or per-consumer caps) and
R4-F2's pressure-driven half (the exit-status ABI). Withdrawn as verified: the
trim path against dlmalloc's own arithmetic, no merging, the release length, the
unreachable mmapped paths, the routing both ways across 256 KiB, the counting
order, Send and Sync, the manual harness, the six caps, and the streaming's
equivalence with the whole-input answers. The verbatim report and dispositions
are the repo's untracked `memory/audit_b1c_closed_list.md`.
