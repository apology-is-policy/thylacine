---
id: fnd-noexec-r1-f10
type: fnd
round: adt-noexec-r1
severity: P3
status: fixed
title: "The mirror checker did not pin T_E_NOEXEC"
surface: [sub-pouch-process]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "tools/check-spawn-args-mirrors.py"
created: 2026-11-04
---
## Prosecution

`posix_spawn` passes a refused tail's `-T_E` through as errno, which holds only while the values are Linux's, and the checker's set did not name this one.

## Disposition

Added.
