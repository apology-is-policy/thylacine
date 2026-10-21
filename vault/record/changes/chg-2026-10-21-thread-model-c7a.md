---
id: chg-2026-10-21-thread-model-c7a
type: chg
title: "Thread-model C7a: the debug focus falls back to main, never the newest thread"
date: 2026-10-21
arc: arc-phase6-pouch
commits: ["*(pending)*", "f273fe493"]
touched:
  - sub-kernel-devproc
  - sub-kernel-proc
  - sub-kernel-notes
  - sub-nora-host
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-21
---
A chunk of the thread-model overhaul (`docs/thread-model-review.html`; the Fable
review's F-4, P2, I-39). With no breakpoint or step focus -- a manual stop or an
attach -- the debug surface read the HEAD of `p->threads`, which is the newest
thread, and an exited thread stays there, EXITING, until a live peer reaps it:
in a stopped target, for the whole stop. So `regs` / `fpregs` / `kregs` read -1
although every live thread was parked, and `step` armed the dying thread, so the
step never fired and the resume let the target run free. Now the focus counts
only while it is a live thread of the target, and the fallback is main, the
oldest thread not exiting ([[sub-kernel-devproc]]); NULL when every thread is
exiting, which every caller refuses. One helper defines main for the debug
surface and the caught-note pick ([[sub-kernel-proc]], [[sub-kernel-notes]]).
The frame nora reads is the focus thread, else main ([[sub-nora-host]]). A
separate commit corrects the `set_tid_address` row in ARCHITECTURE.md and the
syscall.h comment, which still described the single-threaded stub (the pid; the
tid pointer ignored; never fails). Audit round 1 (Fable 5.1 reviewing Opus 5.5):
0 P0 / 0 P1 / 0 P2 / 3 P3, all closed -- F1 the in-function EXITING clauses are
now unreachable through the selector and are labelled defense in depth, with the
witnesses re-credited; F2 the remaining "head" claims; F3 the caught_target,
debug_stop and debug_step specs re-run (every cfg as claimed: caught_target clean at 613245 distinct states and its eight buggy cfgs each violating the named property; debug_stop all cfgs; debug_step clean at 146 states, runs_free violating Safety, death_lost violating EventuallyAllDead). Gates: the ci suite 2024/2024; three red runs, each failing exactly the legs it targets; the SMP matrix 50/50 with 0 corruption.
