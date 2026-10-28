---
id: chg-2026-10-22-thread-model-c3
type: chg
title: "Thread-model C3: each Proc's threads under their own lock"
date: 2026-10-22
arc: arc-phase6-pouch
commits: ["58eca366a", "73ba2d90a"]
touched:
  - sub-kernel-proc
  - sub-kernel-thread
  - sub-kernel-death
  - sub-kernel-jobctl
  - sub-kernel-notes
  - sub-kernel-devproc
  - sub-kernel-devctl
  - sub-kernel-fault
  - sub-kernel-syscall-dispatch
  - sub-kernel-ninep-attach
established: [lock-proc-threads]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-22
---
A chunk of the thread-model overhaul (`docs/thread-model-review.html`; Fable's
E-6). One global lock with interrupts off was taken twice per spawn, once per
exit and per tkill, and across every `/proc` read. Each Proc now keeps its own
thread lock ([[lock-proc-threads]], ARCH 7.3.1): the thread list, its counts, a
live count, every `THREAD_EXITING` commit, the debugger's focus and the
per-Proc walks. A spawn and a non-last exit take no global lock. Spawn builds
the thread unlinked and admits it in one hold (claim, cap, link), so the thread
cap is exact and no thread joins a dying group. The last thread out decides
under the thread lock, drops it for the handle close, and commits nested in the
global hold that publishes ZOMBIE. Spec-first by operator vote:
`specs/thread_reap.tla` ([[spec-thread-reap]]) landed before the code. The exit's
clear-child-tid store stays after the commit (exec's sole-thread check relies on
it) and the FILE page-in refuses a thread past its commit
([[sub-kernel-fault]], [[seam-exiting-tails-never-sleep]]). Found on the way:
both spawn arms read the new tid after `ready()` (fixed). Audit round 1 (Fable
5.1 reviewing Opus 5.5): 0 P0 / 0 P1 / 1 P2 / 4 P3, all addressed -- F1 the
section-28 cells re-pointed, F2 comments, F3 a test terminal masks IRQs, F4 the
seam records the lost-wake hazard, F5 a test that both spawn arms decide at the
admit. Gates: ci suite 2027/2027 then 2028/2028; sabotage sets A-D, each run reading exactly its expected reds; the SMP matrix 50/50 with 0 corruption; thread_reap, death_wake, caught_target, debug_stop and debug_step every cfg as claimed.
