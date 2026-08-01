---
id: moc-kernel-memory
type: moc
title: "Kernel memory: the physical allocator stack"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
Where every kernel byte comes from. Two layers:

- **[[sub-kernel-mm-phys]]** — pages. DTB-driven bootstrap (five
  reservations, the 8 GiB direct-map cap, the #808 boot page-map),
  the Knuth buddy, and the #807 per-CPU magazines in front of it.
- **[[sub-kernel-mm-slub]]** — objects. Embedded-freelist slabs over
  the buddy; `kmalloc`/`kfree` and the typed caches every kernel
  struct lives in.

This area deliberately guards **no numbered §28 invariant** — the
same honesty as [[inv-i17]]'s prose strength, in the opposite
direction: the invariant table starts above the allocator, but the
audit-trigger table lists `mm/` in its own right because everything
above it assumes allocation correctness silently. Its verification
floor is the boot smoke's exact-count round-trip, UBSan, and the
multi-boot SMP gate.

The area's recurring trap is **`struct page.refcount`** — an alloc
marker (buddy) and an inuse count (SLUB), never the BURROW share
count it resembles ([[sub-kernel-mm-phys]] carries the full warning).

What is NOT here: the virtual side. VMAs, Burrows, demand paging, the
overcommit model, and the MMU live with their own areas (unswept as
of batch 9); `mm/vmo_pages.c` — named by CLAUDE.md's audit table —
**does not exist in the tree** (the audit-trigger row carries a
phantom file; the Burrow page machinery lives in `kernel/burrow.c`).

Locks: [[lock-buddy-zone]] · [[lock-kmem-cache]] ·
[[lock-cache-list]].
