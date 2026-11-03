---
id: adt-note-wait-r1
type: adt
title: "sigsuspend and SYS_NOTE_WAIT round 1, before the first boot"
date: 2026-11-03
scope: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-abi, sub-pouch-signal]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 7}
findings: [fnd-note-wait-r1-f1, fnd-note-wait-r1-f2, fnd-note-wait-r1-f3, fnd-note-wait-r1-f4, fnd-note-wait-r1-f5, fnd-note-wait-r1-f6, fnd-note-wait-r1-f7]
round-of: chg-2026-11-03-note-wait
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 99e35300a with the libc as it will be built, base be5564a23. Nothing had been built or booted, so every outcome in it is derived. Six observations and twelve withdrawn suspicions are in the closed list. F2 and F5 were fixed before the report returned, F6 is deferred to the operator, and the rest are fixed. Not dirty: no round 2 owed unless a run says otherwise.
