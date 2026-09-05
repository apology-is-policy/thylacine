---
id: fnd-25-r1-f1
type: fnd
title: "The lazy page-entry array is one order-9 contiguous alloc — can fail under buddy fragmentation"
round: adt-25-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: [inv-i38]
seam: seam-larder-lazy-array-robustness
created: 2026-07-31
---
## Prosecution

The heap-lazy entry array is a single contiguous kmalloc (the page array
routes to an order-9 buddy block). Under a fragmented buddy the alloc
fails → `page_ensure` returns false → the client silently serves as a
PURE MISS. I-38 holds (no correctness impact), but the cache is
invisibly disabled — a perf cliff indistinguishable from a regression.

## Disposition

Deferred to the seam (a chunked/non-contiguous entry pool is the v1.x
robustness fix). The failure SELF-HEALS: the alloc is re-attempted on
the next install, so the cache comes back when fragmentation clears;
correctness never depends on a fill. Documented at the constant.
