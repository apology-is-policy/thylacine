---
id: fnd-c9-r1-f2
type: fnd
round: adt-c9-r1
severity: P3
status: fixed
title: "Sabotage S07's red depended on where the link put a page table"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c9-r1
regression: "none: a fixture"
created: 2026-11-04
---
## Prosecution

S07 tested the low 16 bits of TTBR0 as if they were the ASID. Those are the root's address bits, and a root is 4 KiB aligned, so bits 15:12 are the layout's. A kernel root on a 64 KiB boundary classifies correctly by accident, and the sabotage's outcome would be a coin.

## Disposition

The mask is now the ASID's top byte, zero for the first 255 spaces, so user threads read as kernel threads every time.
