---
id: dec-2026-10-18-anon-mapping-merge
type: dec
title: "Adjacent anonymous mappings merge, by extending the neighbour's Burrow"
date: 2026-10-18
status: standing
decided-by: user-vote
affects: [sub-kernel-vma, sub-kernel-burrow, sub-kernel-pagemap, sub-kernel-addrspace, spec-capacity]
created: 2026-10-19
---
## Fork

On 2026-10-15 libxml2's `testparser` failed "out of memory" on the
device in `testHugePush`, which push-parses a million elements into a
DOM of about two million nodes. The program had about 269 MiB resident,
and neither its page budget nor the user pool was near its bound. The
cause is the VMA cap. Pouch has no `brk`, so every anonymous `mmap` is
one `SYS_BURROW_RESERVE`, and each reservation mints one Burrow and one
VMA. mallocng maps each one-page group on its own: an `xmlNode` falls in
its 128-byte size class, 30 to a page. Two million nodes need about
66,700 groups, and `PROC_VMA_MAX` is 65536, so the next mapping is
refused (`vma_insert_in` answers `-T_E_NOMEM`, `mmap` fails, `malloc`
returns NULL). A probe print confirmed the refusal at a VMA count of
exactly 65536. Every Pouch program's small-object heap stops near
256 MiB the same way. Linux never reaches its own limit here, which is
the same size (`vm.max_map_count`, 65530), because it merges adjacent
anonymous mappings into one VMA.

The VMA list is also a sorted linked list, so each fault and each
reservation walks it, and a heap that grows one page at a time slows
quadratically (`testparser` ran for about 30 seconds before the
refusal).

## Research

- **Plan 9 (heritage).** There is no `mmap`. The heap is one segment,
  the BSS, which `ibrk` grows, and its page map grows with it. A heap
  is one growing object.
- **Linux.** An anonymous private mapping's `vm_pgoff` is its own
  address in pages, so adjacent anonymous mappings have contiguous
  offsets by construction. `vma_merge` folds a new mapping into a
  neighbour that has the same flags and a compatible `anon_vma` (a
  fresh mapping has none yet). The 2004 `anon_vma` work notes that
  merges of fresh `mmap` and `brk` regions are always safe, and that
  filling a hole fails only where the offsets disagree, which happens
  after `mremap`.
- **FreeBSD.** When a new anonymous entry abuts the previous one,
  `vm_map_insert` asks `vm_object_coalesce` to extend the previous
  entry's object over the new range. It refuses an object that shadows
  another or is mapped elsewhere at other offsets (`OBJ_ONEMAPPING`),
  removes any stale pages in the reused range, and grows the object.
  The entries are then merged with their neighbours
  (`vm_map_simplify_entry`).
- **Allocators.** glibc, jemalloc and scudo carve small objects from
  large regions they map themselves. mallocng alone relies on the
  kernel to merge its many small maps.
- **The tree.** Every `ANON_LAZY` Burrow drops its construction handle
  once mapped, `burrow_share_into` admits only eager ANON and the DMA
  subtypes, Loom registers only eager ANON buffers, and a fork gives the
  child its own clone. So an `ANON_LAZY` Burrow is reachable from one
  address space only. Its mappings keep one relation between address and
  offset: a split or a trim keeps every surviving page's offset, a clone
  maps at the parent's addresses, and the kernel has no `mremap`. The
  range detach releases a slot's page before the slot stops being mapped
  (`specs/capacity.tla`, `NoOrphan`), so an offset that no mapping
  covers holds no page. The protect path already folds adjacent pieces
  of one Burrow back together (`reprotect_mergeable`).

## Options

1. **A Pouch heap arena.** mallocng's maps come from large lazy
   reservations, carved page by page and decommitted when freed. The
   kernel is unchanged and only malloc's private maps move. This was the
   session's recommendation.
2. **The kernel merges adjacent anonymous mappings.** The Linux answer:
   every program that maps memory in small adjacent pieces benefits,
   whatever its allocator. Invariant-bearing: the Burrow object model
   (I-7), copy-on-write sharing (I-44) and I-32's VMA and page charges.
3. **A VMA tree.** Lookups become O(log n), and the VMA cap can follow
   the page budget. This fixes the slowdown, not the count.

## The call

Option 2 (operator, 2026-10-18). `SYS_BURROW_RESERVE` extends a
neighbouring lazy anonymous mapping with the same prot and flags instead
of inserting a new one, and unmap and protect split it as they split any
mapping. Option 3 was not chosen. The O(n) list stays an owned hygiene
item, enqueued on its own (OPEN-BUGS, 2026-10-19).

The mechanism, as designed on 2026-10-19 and stated in ARCH 6.5, takes
FreeBSD's shape over the Burrow:

- A lazy anonymous reservation at an address the kernel chose
  (`SYS_BURROW_RESERVE`, and `SYS_BURROW_ATTACH_LAZY` through the same
  core) extends the Burrow of an adjacent mapping when that neighbour
  is `ANON_LAZY`, lies inside the burrow window, and has the same prot
  and the same flags word (the RW ceiling, with no copy-on-write or
  shared-in state). The address is chosen exactly as before; only the
  representation changes.
- A predecessor's Burrow grows at its end. Its pagemap grows the way
  Plan 9's `ibrk` grows a segment's map, and every node the growth adds
  is charged as an install's node is. A successor is extended downward
  only into offsets that an earlier head trim left free, never by
  re-indexing the map. A refill of the hole between two pieces of one
  Burrow folds the three into one mapping.
- A merge charges no VMA, so `PROC_VMA_MAX` bounds distinct runs of
  mappings, as `vm.max_map_count` does on Linux.
- Three properties make an extension sound, and the kernel checks each
  one. One address space: no handle, no share, a clone per fork. One
  bias: every mapping of an `ANON_LAZY` Burrow places offset 0 at the
  same address. The Burrow records that address at its first mapping
  and checks it at every later one, so an extension cannot alias a
  page that another mapping still maps. Zero read: the extension checks
  that the reused offsets hold no page. A page there would break
  `NoOrphan` and is a kernel extinction.
- A mapping flagged copy-on-write never extends, so a fork's shared
  pages are never touched. A `MAP_FIXED` anonymous mapping (the
  loader's bss tail) is a replace at the caller's address and stays its
  own mapping.

## Rationale

The ceiling belongs to the platform, not to one allocator. A kernel
merge serves every program that maps memory in small adjacent pieces,
and a ported program then meets the VMA limit where it would on Linux,
after the same merges. Extending the neighbour's Burrow keeps the object
model as it is (a VMA still maps one Burrow at one offset), so the
detach, the protect, the fork's clone and the fault path keep their
rules. Only one new operation is needed: growing a Burrow's slot table
at its end. The three properties the extension depends on already hold
in the tree, and the design records each one where it can be checked,
so a later path that broke one would be caught where it happens.
