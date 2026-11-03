---
id: adt-thread-c4-r1
type: adt
title: "Thread-model C4 (thread-directed notes) round 1"
date: 2026-11-02
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-thread, sub-kernel-rendez, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-pouch-thread]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-thread-c4-r1-f1, fnd-thread-c4-r1-f2, fnd-thread-c4-r1-f3, fnd-thread-c4-r1-f4, fnd-thread-c4-r1-f5]
round-of: chg-2026-11-02-thread-model-c4
created: 2026-11-02
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on 47f3f8c08. Not dirty. F1 reverted a decision taken to make a test pass rather than from the memory model; removing the narrowing put every pinned model count back exactly. Enumerating the stores while applying F4 found `SA_RESETHAND`, which neither the audit nor the fix plan listed. Read least deeply: devnotes.c past its kill check, the arch EL0 tail, proc_exec_drop_image_state, cons and 9P EINTR classification, Pouch's __pouch_sig_to_note, the probe's helpers, vivarium_kill_decide.
