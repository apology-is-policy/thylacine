---
id: seam-pouch-spawn-envp
type: seam
title: "`posix_spawn`'s `envp` argument is ignored; the child inherits `/env`"
status: closed
surface: [sub-pouch-process]
opened-by: chg-2026-07-23-cl1b-process
tracker: "CL-1b"
created: 2026-08-01
updated: 2026-10-28
closed-by: chg-2026-10-28-spawn-env
---
## Owed

The child's environment comes from the kernel's `/env` clone
(`env_clone_into` at rfork), not from the `envp` the caller passed. A
program that spawns a child with a DELIBERATELY different environment
gets its parent's instead — silently, since there is no way to report
"I ignored that argument" through the POSIX signature.

Symmetrically, a `setenv` in the parent mutates only the in-process
`__environ` copy, never `/env`, so it does not reach a later child.

## The lift

Decided by operator vote 28 ([[dec-2026-10-28-spawn-env]]): `SYS_SPAWN_FULL_ARGV`'s
`ext_flags` bit 2, `SPAWN_EXT_ENV`, announces a tail naming the child's whole
environment as NUL-terminated `NAME=VALUE` records, and the child's `/env` holds
exactly those in place of the spawner's copy. `posix_spawn` passes `envp` as that
tail. A caller that passes `environ` then hands its child whatever `setenv` changed,
so the second direction above closes too.

The kernel half lands first. This seam closes when pouch's `posix_spawn` sends the
tail. The Go fork and libthyla-rs had the same hole (the fork dropped
`ProcAttr.Env`, and `Command` had no environment setter); they are fixed under the
same decision.
