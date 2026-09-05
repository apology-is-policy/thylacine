---
id: fnd-pty3-r1-f3
type: fnd
round: adt-pty3-r1
severity: P3
status: fixed
title: "The controlling-terminal arms answered the kernel's errno where POSIX specifies ENOTTY"
surface: [sub-pouch-tty]
threatens: []
fixed-by: chg-2026-07-18-pty3
created: 2026-08-01
---
## Prosecution

`TIOCSCTTY` / `TIOCGPGRP` / `TIOCSPGRP` passed the raw fd straight to the
kernel syscalls, so a NON-tty fd (a pipe, a file, a tagged socket)
answered the kernel's refusal errno — `EINVAL` / `EACCES` / `EPERM`-shaped
— where POSIX specifies `ENOTTY`. A program branching on
`errno == ENOTTY` after `tcsetpgrp(0, …)` on a pipe mis-branches.

Not a safety or privilege issue: the kernel independently validates that
the fd is a binding of the caller's controlling terminal, and a tagged fd
is an out-of-bounds handle.

## Fix

A `pts_resolve` pre-gate on all three, for errno fidelity only — the
kernel remains authoritative for real pts fds. Both prosecutors converged
on this one (it was the self-audit's SA-11).
