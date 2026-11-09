---
id: adt-note-wait-r2
type: adt
title: "ppoll's and pselect's mask, sigpending, and the SIGPIPE row, round 2, before the first boot"
date: 2026-11-04
scope: [sub-kernel-notes, sub-kernel-vivarium, sub-kernel-syscall-abi, sub-pouch-signal]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 1, p2: 2, p3: 2}
findings: [fnd-note-wait-r2-f1, fnd-note-wait-r2-f2, fnd-note-wait-r2-f3, fnd-note-wait-r2-f4, fnd-note-wait-r2-f5]
round-of: chg-2026-11-04-poll-mask
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 994735ddf with the libc as it will be built. Its scope took operator vote 28 and vote 25's form, which no round had read. Nothing had been built or booted, so every outcome is derived from the code.

Not a dirty close by the audit skill's rule: no P0, three P1 and P2, and small fixes. The reviewer called it dirty for the P1, and its three red witnesses are real, so the first boot on this code is what proves the fix. Its observations and what it did not read are in the closed list.
