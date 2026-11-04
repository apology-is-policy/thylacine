---
id: adt-thread-11c-r1
type: adt
title: "Item 11c round 1, before the first build"
date: 2026-11-03
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-ninep-client, sub-kernel-ninep-dev9p, sub-kernel-pipe, sub-kernel-poll, sub-kernel-torpor, sub-kernel-devsrv, sub-kernel-stalk, sub-pouch-signal]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-thread-11c-r1-f1, fnd-thread-11c-r1-f2, fnd-thread-11c-r1-f3, fnd-thread-11c-r1-f4, fnd-thread-11c-r1-f5]
round-of: chg-2026-11-03-item-11c
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, in a frozen worktree at 04fd1b409, before anything had been built. Not dirty. F5 is fixed differently from the reviewer's proposal: its drop at the tail would have changed every native program, so Pouch's constructor writes a default row for `child_exit` instead, which makes every Pouch image governed from birth. That change is why a second round ran. It could not reach `stratumd` or `janusd`, which sit in other trees; my census of them says they retry on EINTR, read and not run.
