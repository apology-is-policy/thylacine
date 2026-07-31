---
id: adt-25-r1
type: adt
title: "Task #25 round: the O(1) page-cache rewrite (heap-lazy + hash + CLOCK)"
date: 2026-07-10
scope: [sub-kernel-larder]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-25-r1-f1, fnd-25-r1-f2, fnd-25-r1-f3]
round-of: chg-2026-07-11-fid-lifecycle
created: 2026-07-31
---
The focused round on the page cache's move from a 512-slot inline O(N)
array to the heap-lazy 8192-slot chained-hash + free-cursor + CLOCK
shape (coherence contract byte-identical — only the index and eviction
mechanics changed; the work landed inside the fid-lifecycle keeper
commit). Opus-4.8-max prosecutor + a concurrent self-audit, CONVERGED:
both found the same F1/F2 non-defects, no soundness bug. Verified sound
(do-not-re-litigate): the "valid slot ⟹ linked in its CURRENT key's
bucket" invariant after every op; the evict's unlink-before-rekey under
one hold (no serve observes the intermediate); chain acyclicity + CLOCK
termination ≤ 2×cap; no UAF/double-free/leak/OOB (buffers retained
across evict, freed once at destroy); the free-cursor vs CLOCK
disambiguation of fresh/phantom slots; single lock/unlock per op with
non-blocking kmallocs only. SMP gate 20/20 (default+UBSan × smp4, the
go build as concurrent stress). All three P3s dispositioned in larder.h;
F3 (the O(cap) invalidate) was then CLOSED by the task-#29 qid-hash
before the keeper committed.
