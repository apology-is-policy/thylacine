---
id: fnd-noexec-r1-f3
type: fnd
round: adt-noexec-r1
severity: P1
status: fixed
title: "An interpreter that is itself dynamic answered EACCES"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "execve.interpreter_says_why: an interpreter that is itself dynamic"
created: 2026-11-04
---
## Prosecution

`exec_resolve_from_namespace_ex` leaves `*errp = T_E_ACCES` on its success path, and the load read `interp_err` after a successful resolve.

## Disposition

The load resets it to `T_E_NOEXEC` on a success, and the resolver's contract says the value means nothing there.
