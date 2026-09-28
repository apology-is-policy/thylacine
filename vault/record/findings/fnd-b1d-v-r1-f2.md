---
id: fnd-b1d-v-r1-f2
type: fnd
title: "a flagless mount at a file point still stacked a second member, a union the resolver searches as a directory"
round: adt-b1d-v-r1
severity: P2
status: fixed
surface: [sub-kernel-territory, sub-kernel-syscall-dispatch]
threatens: []
fixed-by: chg-2026-09-25-b1d-v-emount
regression: "sys_mount.refuses_all_but_mrepl_at_a_file; alloc-smoke U-2f file-point leg"
created: 2026-09-25
---
## Prosecution

The first vote's wording refused `MBEFORE` / `MAFTER` at a point that is not a
directory, paraphrasing Plan 9's `order != MREPL`. Plan 9's flag 0 is `MREPL`;
Thylacine's flagless mount appends. So a flagless mount (or `MCREATE`,
`MNOEXEC` or `MPHENO_LINUX` alone) of a second file at a file point that
already hosts one passed the check, and `mount()` appended it. `stalk` treats
two or more members as a union, searched as a directory, whose listing skips
members that are not directories: the mount returned 0 and never showed.

## Disposition

Escalated; the operator voted for option 1 of 4 on 2026-09-25
([[dec-2026-09-25-mrepl-only-at-a-file]]): at a point that is not a directory
only `MREPL` is accepted. `sys_mount_for_proc` checks
`!(flags & MREPL)` there. The kernel test refuses seven flag sets and a
flagless mount beside a mounted file, with two `MREPL` controls; alloc-smoke's
device leg refuses `/bin/joey` over `/bin/system.key` without `REPL` and reads
joey's bytes through the key's name under it.
