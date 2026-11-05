---
id: fnd-noexec-r1-f1
type: fnd
round: adt-noexec-r1
severity: P1
status: fixed
title: "Three kernel witnesses still pinned -1 where the load now answers NOEXEC"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "execve.native_rejects_dynamic_linux and its two twins, moved"
created: 2026-11-04
---
## Prosecution

`test_exec_native_rejects_dynamic_linux`, `test_exec_load_failure_leaves_phenotype` and B-1d's control pinned `-1` for an unresolved PT_INTERP; `exec_load_body` answers `-T_E_NOEXEC`.

## Disposition

Moved to `-T_E_NOEXEC`, the chunk's stated contract. The suite would have gone red on its first run.
