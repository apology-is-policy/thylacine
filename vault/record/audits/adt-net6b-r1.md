---
id: adt-net6b-r1
type: adt
title: "net-6b round 1 (the dev9p.poll bridge)"
date: 2026-06-18
scope: [sub-kernel-ninep-dev9p-poll]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 4}
findings: [fnd-net6b-r1-f1, fnd-net6b-r1-f2, fnd-net6b-r1-f3, fnd-net6b-r1-f5]
round-of: chg-2026-06-18-net6b-poll-bridge
created: 2026-07-31
---
Opus-4.8-max prosecutor (MODEL start==end) + concurrent self-audit. The
self-audit traced the single-client pump exhaustively and stopped there;
the prosecutor found the multi-client starvation (F1) -- the
two-prosecutors-catch-different-things value, recorded. DIRTY because the
F1 fix restructured the kthread pump loop. F4 (P3, the pouch ready-fd
ABA -- task #222) is in the round's counts but its surface (the pouch
patches) has no vault node yet; it backfills with the pouch sweep.
Spec gate re-verified at the close (net_poll clean + BUGGY_LOST_READY +
liveness).
