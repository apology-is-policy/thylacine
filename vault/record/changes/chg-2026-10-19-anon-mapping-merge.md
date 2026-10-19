---
id: chg-2026-10-19-anon-mapping-merge
type: chg
title: "Adjacent anonymous mappings merge; every unmap releases a lazy mapping's pages; a namespace reader keeps only the struct (B-3 vote 2 + SA-5)"
date: 2026-10-19
arc: arc-boosty
commits: ["6d88ffc4d", "d877c83da", "b748d309f", "38b1333aa", "a7481dfb5", "582de233f", "da99e6c8e"]
touched:
  - sub-kernel-vma
  - sub-kernel-burrow
  - sub-kernel-pagemap
  - sub-kernel-syscall-dispatch
  - sub-kernel-protect-witness
  - spec-capacity
  - sub-kernel-territory
  - sub-kernel-devproc
  - sub-kernel-death
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-19
---
Vote 2 of B-3 (operator vote 2026-10-18,
[[dec-2026-10-18-anon-mapping-merge]]): the Pouch heap hit `PROC_VMA_MAX`
near 256 MiB because mallocng maps one group per VMA, and the operator chose
a kernel merge over a heap arena in Pouch. A lazy anonymous reservation the
kernel places next to a qualifying lazy mapping now extends that mapping's
Burrow instead of minting a Burrow and a VMA. The same scope carries two
fixes found on the way: the exact-match unmap released no lazy pages, which
the merge would have turned into a user-triggerable extinction, and
`/proc/<pid>/ns` held a strong Territory ref (SA-5, from the votes 5-7
round), so a reader could keep a dead Proc's mounts alive.

**What.** [[sub-kernel-syscall-dispatch]]: `lazy_reserve_core` is the one
body of `SYS_BURROW_RESERVE` (124, and the phenotype's anonymous `mmap`) and
`SYS_BURROW_ATTACH_LAZY` (83). Under `as->lock` it places the range first-fit,
tries `vma_extend_anon_in`, and mints only when nothing qualifies. The
construction handle drops in the same hold, so the shape of a two-thread
`mmap` loop does not depend on scheduling. [[sub-kernel-vma]]: `extend_admits`
accepts exactly a reservation's own shape: `ANON_LAZY`, the same prot, a
flags word equal to the RW ceiling with no state bit (no seal below it, no
COW routing, not shared in), and wholly inside the burrow window. The
predecessor grows forward. The successor grows backward only into offset room
a head trim left. `reprotect_merge_in` folds a hole refill back into one
mapping. [[sub-kernel-burrow]]: `burrow_lazy_extend` refuses a Burrow a
handle names, one whose mapping count exceeds its mappings in this space, or
one mapped at a second bias. It checks that the slots it hands out hold no
page (an extinction if one does: NoOrphan broken). It then grows the pagemap.
`burrow_lazy_bind_va0` records the bias when `vma_insert_in` links a mapping.
[[sub-kernel-pagemap]]: `pagemap_grow` takes a map to a larger count in
`pagemap_init`'s own shape for that count: a larger inline leaf, inline slots
chained under a radix, or new top levels. Every node is charged before it is
allocated and nothing is allocated under the lock. `pagemap_next_resident`
finds a resident slot without taking it. A merge charges no VMA, so
`PROC_VMA_MAX` bounds distinct runs of mappings, as `vm.max_map_count` does
on Linux.

**The unmap door (OPEN-BUGS 20:36Z, pre-existing).**
`burrow_unmap_reporting_in` removed a lazy mapping without releasing its
slots, and two of its callers re-take `as->lock` with no identity check (Loom's
ring setup failures and weft's map). So a sibling thread could have its lazy
reservation unmapped in the ring's place, leaving resident orphans. Before the
merge that leaked a charge. With it, a later extension over those offsets would
have extincted. The unmap now releases the slots, and the five re-taking
callers go through `unmap_if_still_ours`.

**SA-5.** [[sub-kernel-territory]]: a Territory carries strong refs (Procs)
and weak refs (the `/proc/<pid>/ns` reader, [[sub-kernel-devproc]]). The last
strong drop detaches the mount table and `root_spoor` under `ns_lock` and the
dot path under `dot_lock`, and releases them outside both. The last weak drop
frees the struct. A Proc's mounts are released at its death on its own thread
([[sub-kernel-death]]), whoever is reading its namespace.

**The bias stamp (lease 4).** The first build stamped the bias in `vma_alloc`.
`burrow.map_fixed_refusals` extincted at boot: a VMA the replace allocated and
then refused had stamped a bias, and the test's next fixed map read as a second
bias. The stamp moved to `vma_insert_in` (582de233f). A second bias now
disqualifies the Burrow from extension instead of extincting, since a kernel
caller may map one lazy Burrow twice and only the merge needs one bias.

**Model.** `specs/capacity.tla` (d877c83da) is now over addresses, with
`Extend`, `NoAlias` and `ZeroRead` ([[spec-capacity]]). `capacity.cfg` pins
625 distinct and 5785 generated states. Without `Extend` the count is 5001,
and `capacity_buggy_extend_alias.cfg` fails on `NoAlias` with three
invariants holding ahead of it.

**Audit.** Holotype round 1 ran on Fable 5.1 against the Opus 5.5
implementation, with the same model at start and end:
0 P0, 0 P1, 0 P2, 4 P3, all fixed in da99e6c8e.
- F1: the one-address-space property was only argued; it is now checked by
  the mapping count.
- F2 and F3: documents.
- F4: the handle drop moved into the same hold.

**Witnesses.** Lease 5 ran on the CI image of `1cef21e51`, which is b3
merged over main's AS-R9.
- The suite passed 2044/2044 and test-fault 8/8.
- On the device, libxml2's testparser passes. It is the run that had stopped
  at the VMA ceiling.
- libxml2 passes 22 of 23 tests. runtest's one error is the thread cap,
  which belongs to another chunk: an exited peer thread is never reaped
  while its Proc lives (OPEN-BUGS 15:03Z).
- RA turned red in the 6 tests predicted. It removed the growth and the
  handle check, and gave the ns reader a strong ref.
- RB turned red in the 4 tests predicted. It admitted any flags word,
  dropped the second look, and removed the second-bias and other-space
  refusals.
- RC removed the unmap release and ended in the NoOrphan extinction at
  `anon_merge.unmap_releases_lazy_slots`.
- RD put a tail piece one page off its bias. The hole refill turned red, and
  so did four tests that check a tail piece's bytes.
- The restored kernel passed 2044/2044.
- `capacity` matched its pins, and its three buggy cfgs each violated their
  named invariant. `burrow` (AS-R9's model) and the 23 territory buggy cfgs
  were as claimed.

The tests are `pagemap.grow_keeps_every_page` and ten `anon_merge.*`:
adjacent reserves, growth across every shape, hole refill, successor room,
never across a boundary, refused growth, the fork clone, the unmap release,
the second bias and another space. SA-5's are
`territory.weak_ref_keeps_the_struct_not_the_contents` and the exit leg of
`devproc.ns_admission_after_render`.
