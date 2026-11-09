---
id: dec-2026-11-05-policy-refusal
type: dec
title: "An image the kernel refuses by policy reads EACCES, not ENOEXEC"
date: 2026-11-05
status: standing
decided-by: user-vote
affects: [sub-kernel-exec]
created: 2026-11-05
---
## Fork

Round 2 of the ENOEXEC chunk found that a W+X image or one with an executable stack read `ENOEXEC`. Those are valid aarch64 ELFs that Linux runs, and a POSIX shell re-runs an `ENOEXEC` file as a script. Vote 26 read "not a runnable image", which could take them in or leave them out.

## Decision

Operator vote 33: they read `T_E_ACCES`, from `execve` and `SYS_SPAWN_FULL_ARGV`. ABI signoff for `docs/ERRORS.md`. The alternative was to keep `ENOEXEC` and state the script fallback as a known caveat.

## Consequences

`T_E_ACCES` already covers a W^X violation and a file the mount or the Dev will not execute, so it keeps the sense of "may not". A shell prints Permission denied and runs nothing. The rule is the same for a program and for its interpreter. Other well-formed images the loader cannot take (too many segments, a segment of zero size, a write-only segment) stay `ENOEXEC`. A dynamic program whose own segments break the rules passes the kernel, because `elf_load` stops at its `PT_INTERP`, and its interpreter maps it.
