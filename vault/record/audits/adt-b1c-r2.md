---
id: adt-b1c-r2
type: adt
title: "B-1c (the native heap) round 2: a line's bound did not bound what grep and cut built from it, and cat's line mode held a whole line"
date: 2026-09-24
scope: [sub-thyla-heap, sub-libthyla-rs, sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 1, p3: 8}
findings: [fnd-b1c-r2-f1, fnd-b1c-r2-sa2]
round-of: chg-2026-09-24-b1c-round2-close
prior-round: adt-b1c-r1
created: 2026-09-24
---
## Scope

Branch `b1c-native-heap` at 82b5f0d7 (WIP 8): round 1's fixes, `git diff
f7da956e 82b5f0d7` -- `LINE_MAX` through every streaming consumer, #54's
gone-reader policy across nineteen filters, thyla-heap's alignment routing,
`/heap-probe`'s legs that could not fail, and the round-1 close's docs. The
round ran on Opus 5.5 at max effort, the fallback tier, told that context
independence was what it brought and to re-derive each fix's claim from the
code rather than from the round-1 record.

## Convergence

0 P0 / 0 P1 / 1 P2 / 8 P3 from the round, merged with the main session's
parallel self-audit (SA2-1..SA2-8). SA2-1 is F1; SA2-2 is the round's F6, which
the self-audit rated P2 and the merge takes at the higher severity; SA2-5 is
F3's third part, SA2-6 its fifth and SA2-8 part of F7; SA2-3 and SA2-4 are new;
SA2-7 is withdrawn (it claimed `grep -r` would read `/srv` and `/proc`, and
neither has a directory listing). Dirty by the shape of the fixes -- grep's and
cut's per-line work moved into the library, cat's line mode rewritten, a new
search for grep's verdict, the smoke harness reworked -- so round 3 follows.
F1 ([[fnd-b1c-r2-f1]]) is the finding of the round: `LINE_MAX` bounded the bytes
a filter held, not the lists grep and cut built from them, so the register's
"input-driven half CLOSED" was false. The self-audit's SA2-2
([[fnd-b1c-r2-sa2]]) was the same class in `cat`, whose line mode held a whole
line through the one `BufRead` line reader in native code. The P3s: `grep -c`
after a gone reader stopped at its first operand and exited 1 for files it never
searched (F2); #54's gaps -- `head` and `tail` banners written through the
swallowing `print!`, hexdump's operand loop, pelt's walk and ls's directory loop
never checking, cat and head going on after a write error and blaming the input
(F3); ns's raw path swallowing a write error (F4); cat and head reading an
input's EPIPE as a gone reader (F5); the reader-leaves harness -- a first read
with no deadline, a pass when the filter ended by itself, stderr drained only
after the wait, a header stale since #68 (F7); `grep -r` opening netd's `clone`
and reading its conversations' `data`, which netd types as regular files (F8);
four doc wordings (F9); uniq's first line grown by an amortized reservation
(SA2-3); and `tail -n 0` reading all of an endless input (SA2-4). All fixed
before landing except F8, pre-existing and owed to the operator (a stat change
in netd, OPEN-BUGS). Verified sound by the round: the line bound's exactness
both ways, the long-line release, the run grouping's stop, refusal and
pending-run paths, the tail window's scan, drain and shrink, the alignment
routing's symmetry, `add_direct`'s refusal, `/heap-probe`'s premises, the
`OutSink` latch, grep's statuses and device skip, and that every smoke child is
reaped on every path. The verbatim report and dispositions are the repo's
untracked `memory/audit_b1c_closed_list.md`.
