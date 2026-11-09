---
id: fnd-noexec-r2-f4
type: fnd
round: adt-noexec-r2
severity: P3
status: fixed
title: "The boot's exec_setup failure printed a negative as an unsigned"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-05-noexec-r2
regression: null
created: 2026-11-05
---
## Prosecution

`kernel/joey.c` printed `(u64)rc`, so -12 read as 18446744073709551604. The cast is older than this chunk, and `e76b54650` is what gives that path a real errno to print.

## Disposition

It prints the sign and the magnitude. No witness: it needs an init that fails to load, which ends the boot.
