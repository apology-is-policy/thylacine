---
id: seam-pouch-spawn-envp
type: seam
title: "`posix_spawn`'s `envp` argument is ignored; the child inherits `/env`"
status: open
surface: [sub-pouch-process]
opened-by: chg-2026-07-23-cl1b-process
tracker: "CL-1b"
created: 2026-08-01
updated: 2026-08-01
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

`SYS_SPAWN_FULL_ARGV` carries a per-child environment override as a record
tail: its old `_pad_envp` slot became `ext_flags`, whose bits each announce
a tail after the 104-byte record ([[dec-2026-10-06-spawn-cwd]]; the cwd took
bit 0). A u32 could never have held the vector's address, so the override is
an `ext_flags` bit and its own tail. Landing it also gives `setenv` a
write-back target, which is the half that makes the two directions
consistent.
