---
id: chg-2026-05-24-p6-signals-b
type: chg
title: "P6 sub-chunk 13b: POSIX signals over kernel notes"
date: 2026-05-24
arc: arc-phase6-pouch
commits: ["b528bb0a"]
touched:
  - sub-pouch-signal
established: []
closed: ["fnd-signals13b-r1-f1", "fnd-signals13b-r1-f2", "fnd-signals13b-r1-f10", "fnd-signals13b-r1-f11"]
opened: ["seam-pouch-sigmask-per-thread"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Nine files: the five note syscall numbers, a per-Proc sigaction table, a
per-Thread mask shadow, and ONE bootstrap handler registered at
constructor time that dispatches by note name. Paired with a one-line
kernel change (`SYS_POSTNOTE` accepts `pid == 0` as the self-post
sentinel, which `raise` needs).

[[adt-signals13b-r1]] was DIRTY (2 P1 + 6 P2): the seam-check omission
recurring from the threads round ([[fnd-signals13b-r1-f1]]) and the
multi-thread `NDFLT` refusal that wedged a Thread in `for(;;)` with
`in_handler` still set ([[fnd-signals13b-r1-f2]]). [[adt-signals13b-r2]]
on the fixes converged clean -- and found that R1's own F4 close had
missed two in-file comments still asserting the retired claim.
