---
id: adt-thread-11c-r3
type: adt
title: "Item 11c round 3, on the walk hold"
date: 2026-11-03
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-ninep-client, sub-kernel-ninep-dev9p, sub-kernel-pipe, sub-kernel-poll, sub-kernel-torpor, sub-kernel-devsrv, sub-kernel-stalk, sub-pouch-signal]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-thread-11c-r3-f1, fnd-thread-11c-r3-f2, fnd-thread-11c-r3-f3]
round-of: chg-2026-11-03-item-11c
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, reading only, on e19a6033e, which I found myself while doing the census round 2 owed: a caught note during the walk of an interruptible open read ENOENT. It re-derived that a note queued during a held walk is claimed at the open's wait entry in the reader arm, the non-reader arm and the pipe transport; that death, terminate and a dead session still unwind a held walk; that the flag is given back on every path; and that no kernel test or userspace leg asserts an interruptible walk. It could not reach the EL0 tail path, the handshake wait's shape in detail, or `stratumd` and `janusd` under an open that returns EINTR. A reading round: nothing it predicted had run when it closed.
