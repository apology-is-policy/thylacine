---
id: fnd-c10-r1-f2
type: fnd
round: adt-c10-r1
severity: P3
status: fixed
title: "The stack map and the slot put trusted the caller"
surface: [sub-kernel-mmu]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "sabotage K15 reached the map guard at its first use; the slot put has none, since nothing reaches it without a double free"
created: 2026-11-04
---
## Prosecution

A put of a clear bit, or a map over a valid leaf, went unnoticed.

## Disposition

Both now end in an extinction.
