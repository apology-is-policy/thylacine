---
id: fnd-rw1-af1
type: fnd
round: adt-rw1-mm-r1
severity: P2
status: fixed
title: "kmalloc's page-rounding wrapped near SIZE_MAX — a giant request became a 1-page success"
surface: [sub-kernel-mm-slub]
threatens: []
fixed-by: chg-2026-06-10-rw1-allocator
regression: "the pre-round reject in kmalloc's large path"
created: 2026-08-01
---
## Prosecution

For n within `PAGE_SIZE - 1` of SIZE_MAX,
`(n + PAGE_SIZE - 1) >> PAGE_SHIFT` wraps to a tiny page count: the
caller asked for ~SIZE_MAX bytes and RECEIVED a valid 1-page
allocation. Every later write past 4 KiB is silent corruption. The
wrap also defeats `kcalloc`'s `n * size` overflow guard for
`size == 1` — that check passes, then the wrapped rounding fires.
Non-wrapping oversize n was already safe (order > MAX_ORDER rejects).

## Fix

Reject `n > SIZE_MAX - (PAGE_SIZE - 1)` before rounding. The exact
window, nothing wider.
