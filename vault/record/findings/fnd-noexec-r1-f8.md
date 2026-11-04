---
id: fnd-noexec-r1-f8
type: fnd
round: adt-noexec-r1
severity: P3
status: documented
title: "A good program whose interpreter is no image reads ENOEXEC"
surface: [sub-kernel-exec]
threatens: []
created: 2026-11-04
---
## Prosecution

A shell then runs the program as a script. Linux has ELIBBAD; this registry does not, and adding to it wants a signoff.

## Disposition

Stated as the one knowing exception in `docs/ERRORS.md`. Put to the operator: register `T_E_LIBBAD`.
