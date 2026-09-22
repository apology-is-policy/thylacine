---
id: adt-pouchb0-r4
type: adt
title: "Pouch B-0 round 4 (the kernel poll surface): the re-arm stranded a console poller, the prosecutor's minimal fix would have leaked the secret's cadence, and the loop could not die"
date: 2026-09-21
scope: [sub-kernel-poll, sub-kernel-cons, sub-kernel-srvconn, sub-pouch-net]
reviewer: opus
model-start: "claude-opus-5"
model-end: "claude-opus-5"
verdict: dirty
counts: {p0: 0, p1: 2, p2: 1, p3: 9}
findings: [fnd-pouchb0-r4-f1, fnd-pouchb0-r4-f2, fnd-pouchb0-r4-f3]
round-of: chg-2026-09-21-srvconn-two-endpoint-poll
prior-round: adt-pouchb0-r3
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 36878c94 (code d5c58d76): the kernel poll re-arm, the two-endpoint SrvConn poll, the libc deltas 0039-0041. The FALLBACK tier: the Fable round died of credit exhaustion at its first line and was re-spawned on Opus 5 with the same-family preamble. Read-only; the nine poll cfgs reproduced; scratch TLC models for F1 (NoMissedPoll violated in 11 states; clean with a fix, 98) and F2 (DeathTerminates fails at once).

## Convergence

Dirty: two P1s, one introduced by the re-arm ([[fnd-pouchb0-r4-f1]]) and one pre-existing ([[fnd-pouchb0-r4-f3]]), plus the loop's missing death/stop checks ([[fnd-pouchb0-r4-f2]]). The prosecutor's minimal fix for F1 was rejected on a spec it did not have: re-registering only the frozen hook leaves a pre-SAK poller woken once per secret key byte. The P3s: POLLOUT missing from 0041's shaping, two false header sentences (0041, 0039), 0029 applying at an offset the tool's quiet output hid, the role-vs-readiness EAGAIN spin (documented by design), the one-list-per-conn cost (tracked), two test gaps and a timing anchor, prover exit paths, an audit row's inverted lock claim, and pre-existing timeout-conversion UB (new patch 0042). All fixed, documented or tracked; round 5 follows on the fixes.
