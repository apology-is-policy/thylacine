---
id: seam-torpor-reclaim-uaccess
type: seam
title: "A future page-reclaim pass must re-establish non-blocking uaccess under torpor_lock"
status: open
surface: [sub-kernel-torpor]
opened-by: chg-2026-07-04-torpor-lockfree
tracker: "REVENANT R-5 F1 close; REVENANT section 9"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The R-5 fix (pre-fault the futex word BEFORE `torpor_lock`) is sound
today only because file-backed pages are never individually evicted:
once pre-faulted, the under-lock reload cannot reach the BLOCKING
file demand-page arm. The moment an Image-cache pressure-reclaim (or
any pageout) can evict a resident file page between the pre-fault
and the under-lock load, the load can again sleep on a 9P read while
holding the global futex lock — the system-wide stall R-5 closed.

## The lift

The Linux futex shape: a pagefault-disabled atomic uaccess under the
lock, with drop-lock → fault-in → retry on failure. Whoever builds
reclaim owes this in the same chunk.
