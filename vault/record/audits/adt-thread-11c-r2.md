---
id: adt-thread-11c-r2
type: adt
title: "Item 11c round 2, on the first round's fixes"
date: 2026-11-03
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-ninep-client, sub-kernel-ninep-dev9p, sub-kernel-pipe, sub-kernel-poll, sub-kernel-torpor, sub-kernel-devsrv, sub-kernel-stalk, sub-pouch-signal]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 2}
findings: [fnd-thread-11c-r2-f1, fnd-thread-11c-r2-f2, fnd-thread-11c-r2-f3]
round-of: chg-2026-11-03-item-11c
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, in a frozen worktree at f99b1ad4d, before anything had been built, named at the fixes and at what governance from birth changes. Not dirty. It withdrew governance with a NULL table (every reader is NULL-safe), the exec of a Pouch image, every caller of `notes_row_has_taker`, the rfork ordering states and the in-handler scan's cases. It could not reach every dev9p caller that issues a further non-clunk op after an earlier op returned -EINTR within one call, where a later completed op clears a mark an earlier flushed op rides on: at worst a spurious EINTR under `SA_RESTART`, with no loss.
