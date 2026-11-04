---
id: adt-walk-errno-r2
type: adt
title: "Walk errno round 2, on r1's fixes and the spawn commit"
date: 2026-11-03
scope: [sub-kernel-stalk, sub-kernel-dev, sub-kernel-ninep-dev9p, sub-kernel-syscall-dispatch, sub-kernel-exec, sub-pouch-process, sub-lictor]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 1}
findings: [fnd-walk-errno-r2-f1]
round-of: chg-2026-11-03-walk-errno
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 10e66bef7, scope 6a4a276c9..10e66bef7, before anything had been built. It re-derived from the code: the fused fallback terminates and its one fall-through is safe (dev9p's walk_attrs binds nc only on a non-NULL return, so one detach and unref is right); union_walk_failure's callers; no failed spawn can return 0 or -1 through `-exe_err`, every `*errp` on a NULL return enumerated; the fold stays off walk_attrs, so the fused fallback still degrades; patch 0070 against 0064 and the reconstructed search loop; each new test traced by hand and shown red on the pre-fix code; lictor's miss paths. It could not read libthyla_rs::ninep, the musl tree, ut's PATH search, joey's optional_absent or Stratum's walk_components. Its map of arms no test turns red drove witnesses for the clone arm's fold and the two EACCES arms. READ, NOT RUN.
