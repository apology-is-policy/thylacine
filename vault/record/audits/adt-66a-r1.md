---
id: adt-66a-r1
type: adt
title: "#66a (Spoor.path substrate + SYS_FD2PATH) focused round"
date: 2026-06-12
scope: [sub-kernel-path, sub-kernel-stalk]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 6}
findings: [fnd-66a-r1-f1, fnd-66a-r1-f2, fnd-66a-r1-f3, fnd-66a-r1-f4, fnd-66a-r1-sa1]
round-of: chg-2026-06-12-66a-spoor-path
created: 2026-08-01
---
## Scope

The Path substrate on the just-RW-4-audited hot walk path (commits
`ffd224aa` impl + `b50686c8` close). Fable formal round (MODEL
start==end, no fallback — the fourth security-adjacent surface to stay
on Fable with no filter trip) + a concurrent self-audit.

## Convergence

The central I-33 lever (resolver write-only, set-before-publish,
lifetime subordinate to the Spoor) HELD under every falsification Fable
constructed: the multi-thread fd-close-vs-fd2path race (the #844 ref
transfer pins both Spoor and Path), the transplant-reads-published-probe
path (immutable + pinned), the walk-open extend-after-src-clunk window
(nc's own Path ref). Refcounts balance on all 9 traced
create/destroy/replace paths; I-28 untouched (purely additive hooks).
Fable independently re-derived the entire self-audit SOUND set and
could not falsify any closed-list item. All 6 formal P3s + the
self-audit's SA-1 FIXED in the close. Two stayed body-only (unswept
surfaces): F5 — the walk_CREATE adoption arm was unchecked for a
replacing Dev (unreachable v1.0; the open-arm reject mirrored onto it)
and F6 — a joey probe printed an uninitialized buffer on the failure
branch. Withdrawn-by-Fable set (consistent-with-discipline):
non-atomic diagnostic counters, the (hidx_t) truncation idiom, int-ref
overflow, per-hop Path-alloc DoS (handle-table bounded), control-byte
names (the /proc renderers must treat names as untrusted). The owed F2
adoption-arm regression (`stalk.path_adopt_transplant`, NON-VACUOUS —
the fixture returns a NAMELESS replacement) was delivered at #66b, whose
territory-surface round pends that sweep. SMP gate PASS on both the
substrate and close SHAs.
