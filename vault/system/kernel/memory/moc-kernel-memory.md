---
id: moc-kernel-memory
type: moc
title: "Kernel memory: the physical allocator stack"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
Where every kernel byte comes from, and what names a region once it
exists. Four layers:

- **[[sub-kernel-mm-phys]]** — pages. DTB-driven bootstrap (five
  reservations, the 8 GiB direct-map cap, the #808 boot page-map),
  the Knuth buddy, and the #807 per-CPU magazines in front of it.
- **[[sub-kernel-mm-slub]]** — objects. Embedded-freelist slabs over
  the buddy; `kmalloc`/`kfree` and the typed caches every kernel
  struct lives in.
- **[[sub-kernel-burrow]]** — the memory object. A region independent
  of any address space, with **two** refcounts (handles and mappings)
  because there are two independent ways to reach it; six backing
  types in two families, contiguous and sparse. Guards [[inv-i7]].
- **[[sub-kernel-asid]]** — address-space identifiers. The rolling
  generation cache that replaced a per-Proc permanent allocation
  (which extincted on the 256th concurrent Proc). Guards [[inv-i31]].

The first two layers deliberately guard **no numbered §28 invariant** —
the same honesty as [[inv-i17]]'s prose strength, in the opposite
direction: the invariant table starts above the allocator, but the
audit-trigger table lists `mm/` in its own right because everything
above it assumes allocation correctness silently. Their verification
floor is the boot smoke's exact-count round-trip, UBSan, and the
multi-boot SMP gate. The upper two are invariant-bearing and both are
model-gated.

The area's recurring trap is **`struct page.refcount`** — an alloc
marker (buddy) and an inuse count (SLUB), never the BURROW share
count it resembles ([[sub-kernel-mm-phys]] carries the full warning).

What is NOT here: the rest of the virtual side. VMAs, the demand-page
fault arms, the overcommit model's syscall surface, and the MMU live
with their own areas, still unswept. `mm/vmo_pages.c` — named by
CLAUDE.md's audit table — **does not exist in the tree** (the
audit-trigger row carries a phantom file; the Burrow page machinery
lives in `kernel/burrow.c`).

Locks: [[lock-buddy-zone]] · [[lock-kmem-cache]] ·
[[lock-cache-list]] · [[lock-burrow]] · [[lock-asid]].
