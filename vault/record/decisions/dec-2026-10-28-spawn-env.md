---
id: dec-2026-10-28-spawn-env
type: dec
title: "A spawn may give its child a different environment, in a record tail"
date: 2026-10-28
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-content, sub-libthyla-rs, sub-pouch-process]
created: 2026-10-28
---
## Fork

A spawned child's `/env` was always a copy of its spawner's (`env_clone_into`
at rfork). Nothing could ask for a different one. Three callers asked anyway,
and each request was dropped without a word:

- The Go fork's `startProcess` dropped `ProcAttr.Env`. `os/exec` always fills
  it, so every Go child got its spawner's kernel `/env` instead. `os.Setenv`
  is process-local, so a variable a Go program set never reached its children
  either.
- Pouch's `posix_spawn` ignored `envp` ([[seam-pouch-spawn-envp]]).
- libthyla-rs `Command` had no environment setter at all.

What was lost: gopls's per-call `GOFLAGS`, `GOWORK` and `GOPROXY=off`;
`go generate`'s `GOFILE` and `GOPACKAGE`; and x/telemetry's
`GO_TELEMETRY_CHILD=1`. Without that variable, cmd/go's daily telemetry
sidecar runs as a plain `go "** telemetry **"` command. That sidecar is the
`go` orphaned by a `go` at the first go command of every boot.

## Research

- **Plan 9.** The child sets its own environment. Go's plan9 `forkExec`
  rforks with `RFCENVG` (a fresh, empty group), creates `/env/NAME` for each
  entry, and then execs (`src/syscall/exec_plan9.go:155-216`). A later write
  to a name replaces an earlier one.
- **Every atomic spawn API takes the environment as an argument:**
  `posix_spawn`'s `envp`, Fuchsia's `fdio_spawn_etc` `environ`, and Windows
  `CreateProcess`'s `lpEnvironment`.
- **Thylacine's spawn creates the child fully formed**, so there is no
  in-child window to write `/env` in. The cwd hit the same wall, and its
  answer was a record tail ([[dec-2026-10-06-spawn-cwd]]). That decision
  already named a future `SPAWN_EXT_ENV` tail as this seam's lift.
- **Native `SYS_RFORK` (`RFPROC|RFMEM`) and `SYS_EXECVE` exist** (LINEAGE
  L-2a, L-3b), so the Plan 9 shape is reachable. But held spawns, cap masks,
  allowances and the cwd tail exist only on the spawn path.
- **`exec` already projects a child's `/env` onto its stack as `envp`**
  (`exec_stage_env`) and refuses one past `EXEC_ENV_DATA_MAX` (32 KiB) with
  `T_E_2BIG`. A child given a larger environment could not be exec'd anyway.

## Options

1. A `SPAWN_EXT_ENV` record tail: the spawner names the child's whole
   environment, and the kernel builds the child's group from it in place of
   the copy.
2. The Plan 9 path: Go and pouch rfork with `RFPROC|RFMEM`, the child writes
   its own `/env`, then `SYS_EXECVE`. This needs no record change, but it is a
   second process-creation path in both runtimes, without the features only
   spawn has.
3. The spawner rewrites its own `/env` around a locked spawn and restores it.
   This needs no ABI change, but other threads see the borrowed environment
   meanwhile: the window shape the cwd tail replaced.
4. Leave it a seam.

## The call

The operator voted on 2026-10-28 (vote 28): **the `SPAWN_EXT_ENV` tail**.

Decided as the tail's own design:
- **Bit 2 of `ext_flags`, a 16-byte tail** `{ env_va, env_len, env_flags }`.
  It follows the cwd and thread tails when their bits are set, in bit order.
  `env_flags` must be 0 (-1 otherwise, the unknown-bits rule).
- **The block is the argv packing:** NUL-terminated `NAME=VALUE` records,
  the same shape `exec` projects as `envp`. `env_len` 0 is an empty
  environment. A non-empty block must end in a NUL.
- **The child's `/env` is exactly the block.** Nothing of the spawner's
  environment carries over. A later record for a name replaces an earlier
  one, as Plan 9's in-child writes and Go's `dedupEnv` do.
- **Built in the spawner, before rfork**, so a refusal makes no child:
  - `T_E_INVAL` for a record with no `=`, an empty name, a name of
    `ENV_NAME_MAX` bytes or more, or a `/` in a name.
  - `T_E_2BIG` for a value past `ENV_VALUE_MAX`, more than `ENV_MAX_ENTRIES`
    names, or a block past `EXEC_ENV_DATA_MAX`.
  - `T_E_FAULT` for unreadable memory, `T_E_NOMEM` when out of memory.
- **The child's thunk installs it** before `exec` projects it and before its
  first EL0 instruction. The rfork copy's group is emptied and refilled in
  place, so a `/proc/<pid>/environ` reader holding the group never sees it
  freed.
- **Ungated.** A spawner could already write any of these variables into its
  own `/env` before spawning, so the tail confers nothing.

## Rationale

The tail is the cwd decision applied a second time. A flag-announced tail
leaves the 104-byte record and the shared Go fork's copy of it where they are.
A kernel that predates the bit refuses it rather than dropping the
environment, and a caller that does not set it is never read past its record.
The Plan 9 path would have given each runtime a second way to make a process,
and that path would lack what only spawn has: the debugger's held launch, cap
masks, allowances and the cwd. Swapping the spawner's own `/env` puts back the
borrowed-state window the cwd tail removed.

The environment's home does not move. Programs still read `/env`, Go's
`goenvs` still reads it, and the tail only sets what the child starts with.

Sequencing: the Go fork is shared by every tree, and a kernel without bit 2
refuses it, so the fork sends the tail only after every tree carries the
kernel half (the cwd-b rule).
