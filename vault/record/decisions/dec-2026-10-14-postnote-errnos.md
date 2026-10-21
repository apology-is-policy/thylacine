---
id: dec-2026-10-14-postnote-errnos
type: dec
title: "SYS_POSTNOTE answers each refusal with its own errno, never a bare -1"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-pouch-signal, sub-libthyla-rs, sub-utopia-eval]
created: 2026-10-14
---
## Fork

`SYS_POSTNOTE` answered a bare `-1` to every refusal, while ARCH 7.6's table
row and its queue-overflow line said it answered `-EINVAL`, `-EPERM`,
`-ESRCH` and `-EAGAIN`. Pouch's seam reads `-1` as `EIO`, so Pouch's `kill()`
of a missing pid reported `EIO` where POSIX says `ESRCH`, and libthyla-rs,
ut's `kill` and the Go port could not tell "no such process" from "not your
child" (OPEN-BUGS, found in sig S-2's land prep).

## Research

- Plan 9: `postnote` fails with an `errstr` naming the reason; the caller can
  read why.
- POSIX and Linux `kill(2)`: `ESRCH` for no such process, `EPERM` for a
  permission refusal, `EINVAL` for a bad signal.
- docs/ERRORS.md: a handler never answers `-T_E_PERM`, which is `-1`, the flat
  sentinel; a permission refusal is `-T_E_ACCES`. The walk already computed
  the reason for the Linux phenotype's `kill` (S-2).

## Options

- **A. Errnos, as ARCH says**: `-ESRCH`, `-EACCES` (the parent gate), `-EAGAIN`,
  `-EINVAL` (a bad, unknown or kernel-only name), `-EFAULT` (an unreadable
  name). An ABI change; Pouch's `kill`, libthyla-rs, ut and Go follow.
- **B. Keep `-1` and correct ARCH.**

## The call

A (operator vote, 2026-10-14, asked with the self-death-snare vote).

## Rationale

The kernel already knew each reason, and every caller above it needed it:
the heritage and POSIX both report it. The permission refusal is `-EACCES`
because ERRORS.md forbids `-EPERM` from a handler; Pouch's `kill` and the
phenotype's spell it `EPERM` at their own boundaries. A child that has died
and is not yet reaped now accepts a post and receives nothing on both paths,
since POSIX counts it a process until it is waited for; the Linux phenotype
had answered that way since S-2, and the walk now owns the rule for both.
