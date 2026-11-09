---
id: fnd-c9-r2-f3
type: fnd
round: adt-c9-r2
severity: P3
status: fixed
title: "`fp_load_regs` leaves compiled code between the V load and the FPCR write"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-05-c9-r2
regression: "none: hardening"
created: 2026-11-05
---
## Prosecution

The function is V-enabled and the compiler does not know q0 to q31 are live after the first asm statement. It may use them for the two loads that follow. The built ELF used W registers, so no such use was seen.

## Disposition

One asm statement for each of `fp_load_regs` and `fp_read_regs`, so nothing compiled sits inside.
