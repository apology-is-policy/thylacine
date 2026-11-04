---
id: chg-2026-11-04-noexec-r1
type: chg
title: "T_E_NOEXEC: round 1's fixes"
date: 2026-11-04
arc: arc-phase6-pouch
commits: []
touched:
  - sub-kernel-exec
  - sub-kernel-burrow
  - sub-kernel-vma
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-pouch-process
  - sub-substrate-build
established: []
closed: [fnd-noexec-r1-f1, fnd-noexec-r1-f2, fnd-noexec-r1-f3, fnd-noexec-r1-f4, fnd-noexec-r1-f5, fnd-noexec-r1-f6, fnd-noexec-r1-f7, fnd-noexec-r1-f9, fnd-noexec-r1-f10]
opened: []
depth: skeletal
created: 2026-11-04
---
adt-noexec-r1's fixes. This corrects chg-2026-11-04-noexec's witness list: the Pouch witness is
pouch-hello-fs's `posix_spawn(noimg)` leg on the writable pool, not pouch-hello-spawn's `noimg` legs,
which could not pass where joey runs them. New: `execve.load_says_nomem_on_a_refused_vma`, legs on
`execve.interpreter_says_why` and `execve.image_check_answers_for_the_parent`.
