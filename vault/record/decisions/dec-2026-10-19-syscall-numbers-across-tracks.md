---
id: dec-2026-10-19-syscall-numbers-across-tracks
type: dec
title: "SYS_SRV_REGISTRY_NEW is 134, whichever branch lands first"
date: 2026-10-19
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-vivarium]
created: 2026-10-19
---
## Fork

Three branches each took the next free native syscall number on their
own. `main` holds `SYS_JIT_CREATE_SEALED` = 127 and
`SYS_ICACHE_SYNC_SPACE` = 128. Corona's branch carries
`SYS_SRV_REGISTRY_NEW` = 127, from Astra's 2026-10-01 work, called by
login. `b3`, not yet landed, holds 129 to 133 (`HANDLE_FLAGS_GET`,
`HANDLE_FLAGS_SET`, `HANDLE_DUP_TO`, `GETPPID`, `UMASK`), with
`VIV_NATIVE_CEILING` = 133 and the phenotype's per-number Linux pins.
Aux's demo merge on 2026-10-19 found the collision. The dispatcher is
one `switch`, so a duplicate fails to compile rather than misroute.

## Research

- **The ABI.** A number on `main` is ABI, and moving it is a format
  break. 127 and 128 are on `main`; nothing past 128 is.
- **The phenotype.** A native number up to `VIV_NATIVE_CEILING` shares
  its value with a Linux syscall, so each new number pins which Linux
  call it shadows (`vivarium.c`), and the ceiling assert names the first
  Linux call above it.

## Options

1. **134 for corona's call**, whatever the landing order: the first
   number no branch holds.
2. **The landing order decides**: whichever branch lands second
   renumbers.

## The call

Option 1 (operator, 2026-10-19; corona agreed). `main` keeps 127 and
128, `b3` keeps 129 to 133, and `SYS_SRV_REGISTRY_NEW` moves to 134 in
corona's five mirrors: the kernel enum, `VIV_NATIVE_CEILING`, libt,
libthyla-rs with login, and `tools/test-srv-registry-abi.py`. The
phenotype pins Linux 134 (`rt_sigaction`) to it, and the ceiling assert
moves to 135 (`rt_sigprocmask`).

## Rationale

A fixed answer lets both branches finish and test against their final
numbers now. Under the landing order, the second branch to land would
renumber after its gates ran. 134 is the lowest number that moves
nothing either branch has built on.
