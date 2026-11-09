---
id: fnd-c10-r1-f5
type: fnd
round: adt-c10-r1
severity: P3
status: fixed
title: "mm/phys.h's reserve ledger was stale"
surface: [sub-kernel-mm-phys]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "none: wording"
created: 2026-11-04
---
## Prosecution

The cache size, the direct map's tables and the range's L3 tables were wrong or missing.

## Disposition

Corrected.
