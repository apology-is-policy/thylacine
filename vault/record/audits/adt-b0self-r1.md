---
id: adt-b0self-r1
type: adt
title: "B-0 self-audit between rounds: a fix wrong on the one Dev class that matters, a '..' that popped a crossed base, and a test that could not fail"
date: 2026-09-21
scope: [sub-kernel-stalk, sub-kernel-poll, sub-pouch-fs]
reviewer: self
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 2}
findings: [fnd-b0self-r1-f1, fnd-b0self-r1-f2, fnd-b0self-r1-f3]
round-of: chg-2026-09-21-mount-shed
created: 2026-09-21
---
## Scope

The author's own verification of the round-3 (libc) and round-2 (shed) fixes on `browser-b0`, 5807bd9f -> d5c58d76: one green boot and one boot per sabotaged kernel (`work/b0/red3/kernel-sab2.py`, modes poll / union / deadline / walkable / floor).

## Convergence

Every item came from a control, not from re-reading. The green boot of the new kernel test failed on the UNSABOTAGED kernel ([[fnd-b0self-r1-f1]]). Reading the `..` arm to fix that showed a second, older defect ([[fnd-b0self-r1-f2]]). The `deadline` sabotage PASSED the test written to pin it ([[fnd-b0self-r1-f3]]). Earlier in the same pass, applying the patch series under `--fuzz=0` showed pouch 0024's file had no trailing newline, so its last hunk applied only through fuzz BSD `patch` never reports; `tools/check-patch-hunks.py` now fails that class. All five sabotages now fail exactly one assertion each, their own.
