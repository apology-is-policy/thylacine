---
id: chg-2026-10-28-spawn-env
type: chg
title: "The spawn environment tail: a spawner names its child's whole environment (env-a kernel + native, env-c pouch)"
date: 2026-10-28
arc: arc-go-ide
commits: ["7bf17edb5", "24e245c26", "3d6a29fb7", "530ddd916", "2805c8d4f", "a2693024e", "d09c5f7fe", "fd0692af2", "a48a5f051", "12ef45a8c"]
touched:
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-content
  - sub-kernel-exec
  - sub-libthyla-rs
  - sub-stratum-boot
  - sub-coreutils-filters
  - sub-pouch-process
  - sub-tyrquake
  - sub-quarry
established: []
closed: [seam-pouch-spawn-envp]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-28
---
No spawned child ever got the environment its spawner asked for: the Go fork
dropped `ProcAttr.Env`, pouch's `posix_spawn` dropped `envp`, and libthyla-rs
`Command` had no setter, so every child got its spawner's `/env` (found when
cmd/go's telemetry sidecar arrived without `GO_TELEMETRY_CHILD`). Operator vote
28 chose a spawn-record tail ([[dec-2026-10-28-spawn-env]]): `ext_flags` bit 2
names the child's whole environment, built by the spawner's kernel before rfork
and installed by the child's thunk before exec projects it, refilling the rfork
copy in place because a cross-Proc `environ` reader holds no reference. env-a
landed the kernel, the libt/libthyla-rs mirrors and `Command::env` (Fable 5.1,
0/0/0/6); env-c made pouch's `posix_spawn` send `envp` (patch 0051), closing
[[seam-pouch-spawn-envp]], and moved the tyr-glquake launcher, which had passed
a NULL `envp` and leaned on the dropped argument, to `setenv` + `environ`. The
Go fork's half (env-b) waits until every tree carries bit 2.
