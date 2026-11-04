---
id: adt-noexec-r1
type: adt
title: "T_E_NOEXEC, round 1, before the first boot"
date: 2026-11-04
scope: [sub-kernel-exec, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-pouch-process]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 4, p2: 1, p3: 5}
findings: [fnd-noexec-r1-f1, fnd-noexec-r1-f2, fnd-noexec-r1-f3, fnd-noexec-r1-f4, fnd-noexec-r1-f5, fnd-noexec-r1-f6, fnd-noexec-r1-f7, fnd-noexec-r1-f8, fnd-noexec-r1-f9, fnd-noexec-r1-f10]
round-of: chg-2026-11-04-noexec
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 796492e86. Each finding was checked against the code before its fix. Nothing has been built or booted.

Not a dirty close by the audit skill's rule: no P0, five P1 and P2, and small fixes. The first boot proves F1, F2 and F3.
