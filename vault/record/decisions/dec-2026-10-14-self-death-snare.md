---
id: dec-2026-10-14-self-death-snare
type: dec
title: "A Proc may end itself with a fault snare through SYS_POSTNOTE self arm"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-syscall-dispatch, sub-pouch-signal, sub-kernel-vivarium]
created: 2026-10-14
---
## Fork

Pouch's `raise()` and `kill(self)` of `SIGSEGV`, `SIGBUS`, `SIGILL` and
`SIGFPE` answered `EINVAL` and the program ran on, where POSIX ends it with
that signal. S-2 fixed `SIGABRT` alone, through `abort()`'s `brk #0xab06`.
The Linux phenotype's `kill` already ends a caller that names itself with
the matching `snare:*` death through `proc_fault_terminate`. How should
Pouch reach that death?

## Research

- POSIX: the default action for each of the four terminates the process,
  and `raise(sig)` delivers it to the caller.
- Linux implements `raise` as `tgkill` to itself; no fault is needed.
- The tree: `viv_kill`'s SELF_DEATH arm calls `proc_fault_terminate(name,
  elr)`; Pouch's `abort()` uses one reserved `brk` immediate, kept clear of
  the immediates clang, gcc, UBSan and WebKit emit.

## Options

- **A. A self-post of the snare**: `SYS_POSTNOTE`'s self arm accepts the
  fault snares when the target is the caller and ends it through
  `proc_fault_terminate`; a snare posted to another Proc stays refused.
- **B. A `brk` immediate per snare**, beside abort's.
- **C. Leave `EINVAL` and document it.**

## The call

A (operator vote, 2026-10-14).

## Rationale

One mechanism for both phenotypes, the principle of the S-2 kill-family
vote: the native call accepts exactly the snares the phenotype's `kill` ends
its caller with (segv, bus, ill, fpe, abort; `notes.self_death_snare` pins
the two sets equal row by row), so neither can drift. No `brk` immediates are
reserved, and the death is the one a real fault reaches. A fault is never
another Proc's to cause, so the cross arm keeps refusing every `snare:*`
name.
