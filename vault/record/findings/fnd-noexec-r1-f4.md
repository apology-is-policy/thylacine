---
id: fnd-noexec-r1-f4
type: fnd
round: adt-noexec-r1
severity: P1
status: fixed
title: "A Vma-slab refusal read ENOEXEC, so a shell would run a good binary as a script"
surface: [sub-kernel-burrow, sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
regression: "execve.load_says_nomem_on_a_refused_vma"
created: 2026-11-04
---
## Prosecution

`vma_alloc`'s NULL for an empty slab reached `burrow_map_in` as `-1`, and the mappers' `-1` is the image's fault by this chunk's rule.

## Disposition

`vma_args_ok` holds the constraint checks, and `burrow_map_in` asks it first, so a NULL past it is `-T_E_NOMEM`.
