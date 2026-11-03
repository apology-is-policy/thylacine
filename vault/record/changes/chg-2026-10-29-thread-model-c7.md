---
id: chg-2026-10-29-thread-model-c7
type: chg
title: "Thread-model C7: one first entry, per-thread /proc, a new thread inherits its creator's mask"
date: 2026-10-29
arc: arc-phase6-pouch
commits: ["f1e4d76f7", "988472ce7", "37b4dbefa", "9149924eb", "b8e953c5c", "3a6ee5461", "19b2b1372", "4d2c3b15e", "d159e3024", "04e52eccb", "febb30c17", "be384162f", "d0d2ae434", "48aa15ee8", "4f9c067f1"]
touched:
  - sub-kernel-exception
  - sub-kernel-thread
  - sub-kernel-sched-smp
  - sub-kernel-sched
  - sub-kernel-devproc
  - sub-kernel-poll
  - sub-kernel-ninep-dev9p-poll
  - sub-kernel-ninep-dev9p
  - sub-kernel-notes
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-exec
  - sub-kernel-birth-hold
  - sub-kernel-death
  - sub-pouch-seam
  - sub-pouch-thread
  - sub-pouch-signal
  - sub-haul
established: []
closed: [seam-poll-dedupe-direction-test]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-29
---
The last chunk of the thread-model overhaul (`docs/thread-model-review.html`,
the Fable review's F-7 and decision 6). Three parts and a vote.

**C7-0, the poll residue.** `poll.dedupe_keeps_the_earlier_hook` and
`poll.dev9p_poll_lock_defers_its_walks` witness what the poll chunk argued
([[seam-poll-dedupe-direction-test]], closed), and `SPAWN_PERM_ALL` gains a
disjointness check.

**C7b, one first entry.** Every first entry to EL0 -- a spawned thread, a forked
one, an exec'd image, a held spawn -- builds an EL0 frame and takes the shared
EL0-return tail (preempt check, die-check, stop leg, notes leg, `KERNEL_EXIT`),
so a thread born into a stop parks before its first instruction and the
hand-rolled erets and register sweeps are gone ([[sub-kernel-exception]],
[[sub-kernel-thread]]). `debug_stop.tla` spawns a thread mid-run
(NoEL0BornStopped, a buggy cfg breaks it).

**C7c, per-thread /proc.** `/proc/<pid>/thread/<tid>/{regs,fpregs,kregs,kstack}`,
`step <tid>` on the Proc `ctl`, and a listable `/proc` (the devproc half of
seam-932) ([[sub-kernel-devproc]]). Operator votes 2026-10-29: no per-thread
`ctl`; readdir in C7; `..` from a pid file to its pid directory.

**Vote V5 (2026-10-29): a new thread starts with its creator's note mask**, from
both spawn arms ([[sub-kernel-notes]]); pouch 0065 restores it in a new thread's
start ([[sub-pouch-thread]]); haul's pumps inherit main's `pipe` mask
([[sub-haul]]).

Audit round 1 (Fable 5.1 reviewing Opus 5.5): 0 P0 / 1 P1 / 0 P2 / 4 P3 -- F1 the
clone did not copy the mask (fixed; it led to V5), F2 departed-thread refusal
legs, F3 5h's test credit, F4 the manual's listing gate (its second half
withdrawn: kstack is not an image file), F5 pre-existing, enqueued. Gates and
round 2: recorded at landing.
