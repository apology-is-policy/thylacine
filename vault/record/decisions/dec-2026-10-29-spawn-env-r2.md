---
id: dec-2026-10-29-spawn-env-r2
type: dec
title: "A spawn may give its child a different environment, in a record tail (restated; the child is born with none)"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-content, sub-kernel-proc, sub-libthyla-rs, sub-pouch-process]
created: 2026-10-29
supersedes: dec-2026-10-28-spawn-env
---
## Fork

This note restates [[dec-2026-10-28-spawn-env]] (operator vote 28). The
decision is unchanged: a spawner names its child's whole environment in a
spawn-record tail. Three statements in that note are not true of the code that
carries it. b3 took the tail by backporting corona's env-a and env-c (operator
vote 33), and its two audit rounds changed the install and the name rule:

- That note's install empties and refills "the rfork copy" in place. Until the
  child's thunk ran, the child held a full copy of its spawner's environment,
  which `/proc/<pid>/environ` would show, a sealed spawner's included. That is
  what a spawn naming a different environment means to withhold.
- Its name rule admitted `.` and `..`, which `/env` can neither reach nor
  remove: every create and unlink refuses them before devenv, and a walk
  resolves them.
- It places the tail after "the cwd and thread tails". A tree without the
  thread model has no thread tail and refuses bit 1.

## Decided

- **Bit 2 of `ext_flags`, a 16-byte tail** `{ env_va, env_len, env_flags }`,
  after the tails of any lower bits that are set, in bit order: record+104, or
  +120 after a cwd tail where bit 1 is refused. `env_flags` must be 0 (-1
  otherwise, the unknown-bits rule).
- **The block is the argv packing:** NUL-terminated `NAME=VALUE` records, the
  shape `exec` projects as `envp`. `env_len` 0 is an empty environment; a
  non-empty block must end in a NUL.
- **The child's `/env` is exactly the block.** Nothing of the spawner's
  environment carries over. A later record for a name replaces an earlier one.
- **Built in the spawner, before rfork**, so a refusal makes no child:
  - `T_E_INVAL` for a record with no `=`, or a name `/env` itself refuses:
    empty, `ENV_NAME_MAX` bytes or more, holding a `/`, or `.` or `..`.
  - `T_E_2BIG` for a value past `ENV_VALUE_MAX`, more than `ENV_MAX_ENTRIES`
    names, or a block past `EXEC_ENV_DATA_MAX`.
  - `T_E_FAULT` for unreadable memory, `T_E_NOMEM` when out of memory.
- **The child is born with no environment.** A tail spawn forks with the
  kernel-internal `RFCENVG` (Plan 9's clean-group flag; `SYS_RFORK` refuses it),
  so the spawner's environment is never the child's, even for an instant. The
  child's thunk adopts the built group before `exec` projects it and before
  the first EL0 instruction. A Proc that already has a group would have it
  emptied and refilled in place, never freed, because a `/proc/<pid>/environ`
  reader may hold it; no spawn reaches that branch.
- **Ungated.** A spawner could already write any of these variables into its
  own `/env` before spawning, so the tail confers nothing.

## Rationale

As [[dec-2026-10-28-spawn-env]]. Being born empty is the Plan 9 shape that
note's design already named (`rfork(RFCENVG)` followed by writes in the child),
made atomic by the spawn that creates the child fully formed. Refilling a copy
was cheaper by one flag, and it left a window that disclosed exactly what the
spawner chose not to pass on.
