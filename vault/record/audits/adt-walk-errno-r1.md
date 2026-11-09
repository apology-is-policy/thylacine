---
id: adt-walk-errno-r1
type: adt
title: "Walk errno round 1, before the first build"
date: 2026-11-03
scope: [sub-kernel-stalk, sub-kernel-dev, sub-kernel-ninep-dev9p, sub-kernel-syscall-dispatch, sub-lictor]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-walk-errno-r1-f1, fnd-walk-errno-r1-f2, fnd-walk-errno-r1-f3, fnd-walk-errno-r1-f4, fnd-walk-errno-r1-f5]
round-of: chg-2026-11-03-walk-errno
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 6a4a276c9, before anything had been built. It read the first commit only: f4495467b, the implementer's self-audit of what flattened the reason after stalk, came after it, and round 2 owes that and these fixes. The mechanism held: the slot is cleared before and read straight after each vtable call, the errno is bounded to [2,4095], and no value has special meaning on the EL0 tail, since the restart gate needs the unwind mark. Every fixture premise it was asked about checked out by reading. It could not reach Stratum's walk_ecode mapping, sys_mount_handler's errno flow, or userspace that reads ENOENT as absence on a 9P mount, which is the shape to watch at the first boot.
