---
id: chg-2026-11-02-thread-model-c4
type: chg
title: "Thread-model C4: a note aimed at one thread"
date: 2026-11-02
arc: arc-phase6-pouch
commits: ["c73f00a9f", "204b44f37", "96ffd0f0f", "799dfaba0", "e24460495", "47f3f8c08", "54726626a", "ce6e929a1", "756aea276", "07635ad4e", "84987124d", "f27c9f4ea"]
touched:
  - sub-kernel-notes
  - sub-kernel-proc
  - sub-kernel-thread
  - sub-kernel-rendez
  - sub-kernel-vivarium
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-pouch-thread
  - abi-note-names
  - spec-caught-target
established: []
closed: []
opened: [seam-queue-note-fatal-after-post-wakes-nobody]
mirrors-checked: [usr/lib/pouch/patches/0007-pouch-signals.patch, usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0049-pouch-sigterm-term-note.patch, kernel/vivarium.c, usr/lib/pouch/patches/0050-pouch-self-fault-signals-errnos.patch, usr/lib/pouch/patches/0067-pouch-thread-directed-kill.patch]
depth: skeletal
created: 2026-11-02
---
`tkill` and `tgkill` posted a process-directed note, so a peer could take a note aimed at someone else, and Pouch's `pthread_kill` hit the ENOSYS sentinel. Each thread now has a word beside the Proc's queue, `Thread.note_direct`, one bit per family, read only by that thread's tail. The tail takes a queued kill first, then the lowest unmasked direct bit, then the queue. A post holds `threads_lock` only, so there is no `q->lock` to `threads_lock` edge. The rouse does not ask the mask: the reader's re-check is the precision. `SYS_POSTNOTE_TID` is native syscall 138 ([[dec-2026-11-02-postnote-tid]]) and Pouch 0067 aims `pthread_kill` and `raise` through it ([[sub-pouch-thread]]). [[spec-caught-target]] carries the words, with a new buggy cfg for the stale mask read.

Audit: [[adt-thread-c4-r1]] (Fable 5.1, 0/0/1/4) and [[adt-thread-c4-r2]] (Fable 5.1, 0/0/0/2 and two observations). Round 1's F1 reverted a mask-gated rouse, the store-buffer litmus test. Applying F4 found `SA_RESETHAND`, a third store that turns a disposition fatal, which neither the finding nor the plan listed. Round 2's F1 is pre-existing, and [[seam-queue-note-fatal-after-post-wakes-nobody]] holds it for item 11c ([[dec-2026-11-02-item-11c-own-chunk]]).

Gates on f27c9f4ea: suite 2169/0, green and regreen with identical ELFs (facf5e46ed8407b2); the guest legs threads-census, kill-worker, kill-masked, raise-masked, kill-blocked, kill-answers; K23, K34, G1 and G7 each red on their own lines. On ce6e929a1: K01-K33, G2-G6, G8 and P1 each red on their own lines. Model: the whole checker 18/18. SMP: `default-smp4` and `ubsan-smp4` each 10/10, 0 corruption ([[dec-2026-11-02-smp-subset-at-c4]]).

NOT RUN: the full SMP matrix, under the operator's hold of 2026-11-02; `tools/test-fault.sh`; the interactive fleet; `tools/check-production.sh`, beyond the production-shape build `tools/test.sh` does first. The merge to main owes them.
