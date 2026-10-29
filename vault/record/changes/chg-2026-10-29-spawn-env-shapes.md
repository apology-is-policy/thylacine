---
id: chg-2026-10-29-spawn-env-shapes
type: chg
title: "The spawn environment tail takes b3's audited shapes: the child is born with no environment, the dot names are refused"
date: 2026-10-29
arc: arc-go-ide
commits: []
touched:
  - sub-kernel-content
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-proc
  - sub-kernel-birth-hold
  - sub-kernel-caps
  - sub-libthyla-rs
  - sub-stratum-boot
  - sub-pouch-process
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-29
---
b3 took the spawn environment tail ([[chg-2026-10-28-spawn-env]]) by
backporting it (operator vote 33), and its two audit rounds found defects that
the origin tree carried too. This change ports their fixes back as new commits,
not a merge. A tail spawn now forks its child with the kernel-internal
`RFCENVG`, so the child is born with no environment and adopts the built one:
before, it held a full copy of its spawner's environment until its thunk ran,
visible to a `/proc/<pid>/environ` reader, a sealed spawner's included. The
name rule refuses `.` and `..`, which `/env` can neither reach nor remove.
Pouch's `__pouch_env_init` (0025) leaves out a name holding `=` and a value
holding a NUL, held by `tools/test-pouch-env-init.py`. `Command::env` and
`env::var` read each value once, so a concurrent writer cannot tear it.
`env_live_count` lets a kernel test see a refused spawn free the group it
built. The env tail's offset is pinned by a static assert on this tree's cwd
and thread tails, and `check-flag-words` now owns the rfork word.
[[dec-2026-10-29-spawn-env-r2]] restates the decision with these and
supersedes [[dec-2026-10-28-spawn-env]].
