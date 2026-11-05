---
id: adt-noexec-r2
type: adt
title: "T_E_NOEXEC, round 2, on the code round 1 never read"
date: 2026-11-05
scope: [sub-kernel-exec, sub-kernel-burrow, sub-kernel-vma, sub-substrate-build]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-noexec-r2-f1, fnd-noexec-r2-f2, fnd-noexec-r2-f3, fnd-noexec-r2-f4, fnd-noexec-r2-f5]
round-of: chg-2026-11-04-noexec
created: 2026-11-05
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at `7f7fab418`. It read what came after round 1's copy: round 1's fixes, `T_E_LIBBAD`, the blob path's map answer and the fixtures. Each finding was checked against the code before its fix. It read and ran nothing else.

It did not read `usr/ambush`, `usr/ut` or `usr/joey`, the Go fork's decode of 80, or devramfs on an empty file, and it read busybox and dash from memory.

Not a dirty close by the audit skill's rule: no P0, one P1 or P2, and small fixes. The boots at the landing's tip prove the fixes.
