---
id: adt-b1b-r1
type: adt
title: "B-1b (the Pouch memory seam) round 1: the chunk's headline defect was a mis-attribution read from one side of the boundary"
date: 2026-09-24
scope: [sub-pouch-mem, sub-pouch-thread, sub-pouch-process, sub-pouch-seam, sub-kernel-vivarium]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 5}
findings: [fnd-b1b-r1-f1]
round-of: chg-2026-09-23-b1b-pouch-memory
created: 2026-09-24
---
## Scope

Branch `b1b-pouch-mem` at 4f76090f plus the working tree of the close: the musl
series' patches 0044 (the mman lower half over `SYS_BURROW_RESERVE` / `PROTECT`
/ `DECOMMIT` / `DETACH`), 0045 (mallocng's MADV_FREE on) and 0046 (`__init_tls`
through `__mmap`), the pthread guard, the 8 MiB main stack with its extent in
auxv, the phenotype madvise rows, and the prover `/pouch-hello-guard`.
Fable 5.1 at max effort, start and end.

## Convergence

0 P0 / 0 P1 / 1 P2 / 5 P3: clean by count, and the fixes were record
corrections, comments, one prover reorder and docs, so no round 2 was owed. F1
([[fnd-b1b-r1-f1]]) is the finding of the round: the defect the chunk announced
as found, `__init_tls`'s raw six-argument `SYS_mmap2` killing Pouch programs,
had never killed one -- the kernel's arm has read that Linux shape since Clade
CL-4. The main session's concurrent self-audit re-read the libc side of that
claim and missed it; the round read the kernel's dispatch arm. The P3s: the
guard prover's positive control was unobservable, its marker going out before
the write that proved the boundary (F2, fixed: the write precedes the marker,
RED with the boundary moved a page down); `MAP_FIXED`'s second call can refuse
after the discard (F3, documented; the atomic form needs a `BURROW_PROTECT` flag
bit, a syscall-interface extension owed to the operator); `USE_MADV_FREE` over an
immediate decommit is churn Linux's lazy free avoids (F4, a design note in
sub-pouch-mem); stale comments (F5, fixed); and three phenotype madvise
divergences from Linux left unstated (F6, documented in VIVARIUM 6.28 and
sub-kernel-vivarium). Withdrawn by the prosecutor as guarded by code: fourteen
lines of attack, among them X degrading to RW, a three-argument protect sealing,
a hint crossing address spaces, and a prover leg a broken system would satisfy
beyond F2. The verbatim dispositions are the repo's untracked
`memory/audit_b1b_closed_list.md`. Recorded after the landing (037f511d,
5ed51ff5), which closed the round without its notes.
