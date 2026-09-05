---
id: adt-29-r1
type: adt
title: "Task #29 round: the 32768-slot working-set cap + the qid-hash invalidate"
date: 2026-07-10
scope: [sub-kernel-larder]
reviewer: fable
model-start: claude-fable-5
model-end: claude-fable-5
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-29-r1-f1, fnd-29-r1-f2, fnd-29-r1-f3, fnd-29-r1-f4]
round-of: chg-2026-07-11-fid-lifecycle
created: 2026-07-31
---
Fable-5-max prosecutor over the page-cache cap lift (8192 → 32768; the
128 MiB lazy ceiling that holds the go-build read working set, pe → 0)
plus the F3 fix: `larder_page_invalidate` O(cap) → O(pages-of-file) via
the `page_qhash` secondary index + `qnext` chain. The first spawn died
on a transient API error mid-analysis (connection closed) — RE-SPAWNED
fresh for a clean full review, MODEL start==end both. Concurrent
self-audit converged on the same verified-sound set: the "in qhash IFF
in page_hash" invariant across all five link/unlink/validity sites; the
qbucket walk-splice (textbook remove-while-iterating, traced through an
interspersed cross-file collision; the nested unlink walks the DISJOINT
hnext chain and cannot corrupt the walk); KP_ZERO honored on both alloc
paths (no garbage-valid false hit); order-9 alloc within MAX_ORDER and
the direct-map cap; the under-lock snapshot copy bounded upstream at
128 KiB independent of cache size; the OOM ladder leak-free with
publish-only-on-full-success. Regression `larder.page_invalidate_
multifile` proven non-vacuous against both over-invalidation and
incompleteness. SMP gate 0 corruption (a first-run 38/40 with 2 OTHER on
the tight default config was RULED OUT by a clean-window re-run 20/20 —
host-timing, guest ground-truthed clean, per the no-host-load
discipline).
