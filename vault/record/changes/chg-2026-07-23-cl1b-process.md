---
id: chg-2026-07-23-cl1b-process
type: chg
title: "Clade CL-1b: the process lifecycle (posix_spawn / wait / pipe)"
date: 2026-07-23
arc: arc-clade
commits: ["1bcabfae"]
touched:
  - sub-pouch-process
established: []
closed: []
opened: ["seam-pouch-dup2-target", "seam-pouch-spawn-envp"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The substrate the toolchain drives: the clang driver posix_spawns
cc1/lld and wait4s them, make forks per job over pipes. Thylacine has no
fork and no execve, so `posix_spawn` cannot clone-and-exec -- instead it
resolves its file_actions STATICALLY against a model of the child's fd
table and emits the positional fd list `SYS_SPAWN_FULL_ARGV` installs.

Two translations are load-bearing and neither is obvious: the wait
option word is mapped bit-by-bit (the kernel's `WAIT_CONTINUED` is 4,
musl's `WCONTINUED` is 8, and the kernel rejects unknown bits), and the
raw exit status is repacked into Linux's layout so musl's `W*` macros
decode it. `pipe` needed a bespoke two-register `svc` shim, since
`SYS_PIPE` returns both fds and musl's `__syscall` captures only one.
