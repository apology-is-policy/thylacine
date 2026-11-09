---
id: dec-2026-11-04-noexec
type: dec
title: "T_E_NOEXEC joins the registry at its POSIX value; execve and the full spawn answer it"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-exec, sub-kernel-syscall-dispatch]
created: 2026-11-04
---
## Fork

A file that is not a runnable image read `EINVAL` from `execve` and the full spawn call, for want
of an `ENOEXEC` in the registry, which is ABI-bearing and wants a signoff. A shell reads `ENOEXEC`
as "run this as a script", so it could not tell a script from a bad argument.

## Decision

Vote 26, the operator, 2026-11-04: add it, as the signoff for docs/ERRORS.md, registered at its
POSIX value; `execve` and the full spawn call answer it for a file that is not a runnable image,
where they read `EINVAL`. A small chunk of its own after the thread model.

## Why

The value is the one every libc already has, so Pouch and a Linux guest read it with no translation.

## What it costs

The answer needs the parent to know before the rfork, so a full spawn reads the image's header
once more. The four older spawn calls keep their flat `-1`. A dynamic binary whose interpreter is
missing still reads a pid and exit 1 from the full spawn, because its interpreter is the child's
load to resolve; `execve` reads the walk's reason.
