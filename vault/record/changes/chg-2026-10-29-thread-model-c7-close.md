---
id: chg-2026-10-29-thread-model-c7-close
type: chg
title: "Thread-model C7 close: round 2, the C11 join fix, the final gates"
date: 2026-10-29
arc: arc-phase6-pouch
commits: ["e4c26c631", "79b3deee8", "4c5a7c57b", "86d1eccaf"]
touched:
  - sub-kernel-notes
  - sub-kernel-devproc
  - sub-pouch-thread
  - sub-pouch-seam
established: []
closed: []
opened: [seam-dirent-type-is-qid-type]
mirrors-checked: []
depth: skeletal
created: 2026-10-29
---
The close of [[chg-2026-10-29-thread-model-c7]], whose record fields were fixed when it was committed. Audit round
2 (Fable 5.1 reviewing Opus 5.5, start == end): 0 P0 / 0 P1 / 0 P2 / 3 P3, all fixed -- the note_mask contract
comments ([[sub-kernel-notes]]), two guest legs past the TLS shadow ([[sub-pouch-thread]]), 5h's errno
([[sub-kernel-devproc]]). Round 1's F5 opened [[seam-dirent-type-is-qid-type]].

The new `mask-c11` leg hung phase f's green boot: a C11 `thrd_create` thread never registered clear_child_tid,
so `thrd_join` waited forever -- pre-existing since pouch 0004. Pouch 0066 makes `start_c11()` register as
`start()` does ([[sub-pouch-thread]]).

Gates on 86d1eccaf (phase f2): suite 2077/0, green and regreen with identical ELFs; the debug-probe threads leg and
the three pouch mask legs pass; S18 (native arm lost, its kernel test unregistered) and S17 (0065's start_c11 hunk
gone) each red on their guest legs; SMP gate default-smp1/4/8 + ubsan-smp4/8 each 10/10, 0 corruption. Phase e on
4f9c067f1: S11, S15, S12, S13, S14, S16 red. Phase d on febb30c17: S1-S10 red, debug_stop and debug_step as
claimed, check-production PASS.
