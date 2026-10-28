---
id: adt-pollwalk-r1
type: adt
title: "Poll hook-list chunk (batched + owed + deferred walks) round 1"
date: 2026-10-28
scope: [sub-kernel-poll, sub-kernel-proc, sub-kernel-ninep-client, sub-kernel-srvconn, sub-kernel-pipe, sub-kernel-loom, sub-kernel-cons, sub-kernel-notes]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 2}
findings: [fnd-pollwalk-r1-f1, fnd-pollwalk-r1-f2, fnd-pollwalk-r1-f3]
round-of: chg-2026-10-28-poll-hook-walks
created: 2026-10-28
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer,
read-only over git objects at d1a0bf094 (aux-3-poll; the working tree was being
rewritten by sabotage runs). Scope 69456f5f8..d1a0bf094: every file and all four
poll specs. Not dirty (no P0, P1 + P2 = 1, the fix one helper): no round 2.
Not audited as deeply as wanted: every simple table-lock acquire/release pair
beyond its diff hunk; `client_lock` inside a non-deferring irqsave hold (argued
unreachable); pts.c's `srvconn_unref` contexts.
