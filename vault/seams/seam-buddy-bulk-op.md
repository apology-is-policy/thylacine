---
id: seam-buddy-bulk-op
type: seam
title: "Magazine refill/drain takes the buddy lock once PER PAGE"
status: open
surface: [sub-kernel-mm-phys]
opened-by: chg-2026-05-04-p1d-phys-allocator
tracker: "HT11.R1-F6"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A magazine boundary crossing moves 8 pages, as 8 separate
`buddy_alloc`/`buddy_free` calls — 8 lock round-trips per crossing,
all inside the caller's IRQ-masked window. The head comment of the
absorbed reference doc claimed the opposite ("each acquisition covers
8 pages amortized"); the code header explicitly names this seam.

## The lift

A buddy bulk-op: pop/push N order-k blocks under ONE hold. Cheap to
write; worth it the first time allocator contention shows in a
profile (the SMP go-build workload is the candidate witness).
