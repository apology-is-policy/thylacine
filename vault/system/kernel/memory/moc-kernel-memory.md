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

The virtual side, swept at batch 29 and living here too:

- **[[sub-kernel-mmu]]** — page tables. The kernel's three views (image,
  direct map, vmalloc) against the per-Proc user root; the PTE encoders
  that make [[inv-i12]] representable; and the two deliberate second
  mappings — the boot patcher's write alias over `.text`, and cross-Proc
  debug access — that write executable memory while keeping W^X true.
- **[[sub-kernel-vma]]** — the address-space description. Small, and
  where [[inv-i12]] is actually **decided**: `vma_alloc`'s `WRITE|EXEC`
  rejection is the single gate every user mapping in the system passes.
- **[[sub-kernel-fault]]** — the dispatcher. Classification (six kernel
  branches, all fatal, each naming its own diagnosis) and demand paging
  across six backing arms, one of which must sleep and does so under a
  pin-and-revalidate protocol.

The three are one story told in three places: **the VMA layer decides,
the fault handler carries the decision unchanged, and the MMU encodes
it.** That is what lets the gate be a single `if` — nothing downstream
re-derives a permission.

Still not here: the overcommit model's syscall surface, and exec's half
of I-36 (task #52), which is why there is no `inv-i36` note yet.

An earlier version of this line said the virtual side was "still
unswept" — written a day before batch 27 declared the subsystem sweep
complete over it. **The corpus contained its own counter-evidence and
nothing compared the two**; see [[chg-2026-08-03-mapping-core-sweep]].

`mm/vmo_pages.c` — named by CLAUDE.md's audit table — **does not exist
in the tree** (the audit-trigger row carries a phantom file; the Burrow
page machinery lives in `kernel/burrow.c`).

Locks: [[lock-buddy-zone]] · [[lock-kmem-cache]] ·
[[lock-cache-list]] · [[lock-burrow]] · [[lock-asid]].
