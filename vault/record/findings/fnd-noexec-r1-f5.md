---
id: fnd-noexec-r1-f5
type: fnd
round: adt-noexec-r1
severity: P2
status: fixed
title: "A refused allocation in the interpreter's argv read E2BIG"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "execve.interpreter_says_why: a rewrite with no memory"
created: 2026-11-04
---
## Prosecution

`exec_interp_argv` returned one NULL for its bound and for kmalloc, and the caller named the bound. `xargs` splits on E2BIG.

## Disposition

It takes `*err_out`: E2BIG for the bound, NOMEM for the allocation, EINVAL for a block no syscall entry would pass.
