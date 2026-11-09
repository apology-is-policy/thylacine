---
id: chg-2026-11-04-noexec
type: chg
title: "T_E_NOEXEC: execve and the full spawn say a file is no image"
date: 2026-11-04
arc: arc-phase6-pouch
commits: [640c4ab2b, 55474ba08]
touched:
  - sub-kernel-exec
  - sub-kernel-syscall-dispatch
established: [dec-2026-11-04-noexec]
closed: []
opened: []
depth: skeletal
created: 2026-11-04
---
Operator vote 26. The loader's failures now say whose fault they are: `-T_E_NOEXEC` for the file,
a failed read's own reason, and a missing interpreter's walk. `exec_image_check` gives the full
spawn its answer before the rfork. Witnesses: execve's four new tests in test_exec.c, viv-pheno-probe
L369 and L370, pouch-hello-spawn's `noimg` legs.
