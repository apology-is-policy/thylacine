---
id: fnd-aux-corona-join-r2-f4
type: fnd
round: adt-aux-corona-join-r2
severity: P3
status: fixed
title: "Nothing pinned the Loom metadata charge to the order kmalloc really takes"
surface: [sub-kernel-loom]
threatens: [inv-i32]
fixed-by: chg-2026-10-29-aux-corona-join
regression: "loom.metadata_charge_matches_kmalloc_order; SJ5 RED"
created: 2026-11-02
---
## Prosecution

`burrow_backing_pages` and kmalloc's large path are two ceil-log2s written apart, and `test_loom`'s charge check restates the code's own formula. Fixed in 009d20767: a test kmallocs every size `loom_create_private` charges plus a three-page block and compares the head page's recorded order.
