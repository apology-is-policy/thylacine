---
id: moc-kernel-memory
type: moc
title: "Kernel memory: the physical allocator stack"
parent: moc-kernel
created: 2026-08-01
updated: 2026-09-23
---
Where every kernel byte comes from, and what names a region once it
exists. Four layers, and the slot table that sits between the object and
its pages:

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
- **[[sub-kernel-pagemap]]** — the sparse Burrow's slot table (B-1a',
  2026-09-23): a 512-ary radix of on-touch node pages, charged to the address
  space that touched them and walked by present nodes -- what lifted the
  reservation cap to the burrow window and made `page_count` read as data plus
  the nodes that index it. Also owns the capacity suite. Guards [[inv-i32]];
  modelled by [[spec-capacity]].
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
  Since B-1a (2026-09-23) also the permission ceiling and the multi-mapping
  reprotect: a mapping's prot moves among {none, R, RW} under a mint-time
  ceiling, and X is never a target. Since B-1a' also the range detach
  (`vma_detach_range_in`), the primitive both detach syscalls and the
  MAP_FIXED replace are built from, whose release-before-reshape order is
  [[spec-capacity]]'s law.
- **[[sub-kernel-protect-witness]]** — the ceiling's three witnesses: the
  in-kernel suite (`test_protect.c`), the EL0 probe, and the expect-fault
  guard child that dies through a page sealed at none.
- **[[sub-kernel-fault]]** — the dispatcher. Classification (six kernel
  branches, all fatal, each naming its own diagnosis) and demand paging
  across six backing arms, one of which must sleep and does so under a
  pin-and-revalidate protocol.

The three are one story told in three places: **the VMA layer decides,
the fault handler carries the decision unchanged, and the MMU encodes
it.** That is what lets the gate be a single `if` — nothing downstream
re-derives a permission.

[[sub-kernel-mm-phys]] carries the user pool (B-1a'; physical since the
round-1 close): RAM minus a TCB reserve, charged at `alloc_user_pages` and
returned at `free_pages`, reclaiming idle images' pages before it refuses
(the round-2 close; [[sub-kernel-image]]), the machine-wide bound above every
address space's cap -- which [[sub-kernel-addrspace]] keeps as the I-32 default and hard
maximum; [[sub-kernel-mmu]] charges and reclaims the hardware page tables
inside it.

Still not here: the overcommit model's syscall surface.

Exec's half of I-36 was swept at batch 31 and lives in
[[moc-kernel-execution]] — so [[inv-i36]] now exists, and the fault
handler's FILE arm holds two of its seven conditions rather than the
whole thing. The other five are exec's dispatch gate, the Image cache's
version key, the charge in this area, and two that Stratum enforces and
this repository cannot check.

An earlier version of this line said the virtual side was "still
unswept" — written a day before batch 27 declared the subsystem sweep
complete over it. **The corpus contained its own counter-evidence and
nothing compared the two**; see [[chg-2026-08-03-mapping-core-sweep]].

`mm/vmo_pages.c` — named by CLAUDE.md's audit table — **does not exist
in the tree** (the audit-trigger row carries a phantom file; the Burrow
page machinery lives in `kernel/burrow.c`).

Locks: [[lock-buddy-zone]] · [[lock-kmem-cache]] ·
[[lock-cache-list]] · [[lock-burrow]] · [[lock-asid]].
