---
id: fnd-noexec-r2-f3
type: fnd
round: adt-noexec-r2
severity: P3
status: fixed
title: "Contracts stated what the code does not do"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-05-noexec-r2
regression: null
created: 2026-11-05
---
## Prosecution

`syscall.h` said the full spawn can answer `T_E_LIBBAD`, and it cannot: the parent's look passes `PT_INTERP`, and the child's failure reads a pid and an exit. `exec_map_segment`'s comment, `exec_image_check`'s contract and ER-2's list were also behind the code.

## Disposition

All four corrected, and ER-2 now names `ELIBBAD`.
