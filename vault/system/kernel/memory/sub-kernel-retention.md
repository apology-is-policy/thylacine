---
id: sub-kernel-retention
type: sub
title: "Retention -- what keeps backing alive, and what pays for it today"
parent: moc-kernel-memory
code: [kernel/test/test_mm0_holders.c]
audit: none
guarded-by: [inv-i7, inv-i32]
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/SHARED-MEMORY-ACCOUNTING.md"]
created: 2026-11-03
updated: 2026-11-03
---
## Purpose

Memory has one owner on paper and several holders in fact. A client's mapping, a
handle, a Loom registered buffer and a Weft share can each keep a page alive after
whoever allocated it has gone. This note says what is true of that today, so the
memory accounts in `docs/SHARED-MEMORY-ACCOUNTING.md` have a measured starting point
and every behaviour they change is changed on purpose.

The accounts now exist and only watch: [[sub-kernel-accounts]]. Everything below is still what decides.

## Contract

The holder table and the allocation classes are section 12 of the design document,
read from the code. Its last column in each table is a proposal, not a decision.

`kernel/test/test_mm0_holders.c` pins four behaviours as they are, none of them
claimed to be right:

- `mm0.today_alias_charges_each_mapping`: two shared-in mappings of one object charge
  `shared_map_pages` twice and the capacity pool once.
- `mm0.today_partial_map_retains_the_whole`: a DMA map cut down to one page still
  holds the whole `KObj_DMA`, charging nothing, and each map's wrapper Burrow is its
  own reference to one allocation.
- `mm0.today_orphan_pin_charges_no_proc`: once the sharer detaches, a client's mapping
  keeps the backing in the pool and on nobody's `page_count`.
- `mm0.today_dma_bypasses_the_pool`: plain, weave and GPU BO backing are raw pages,
  with anonymous backing beside them as the control that moves the pool.

A red in one of these means the behaviour moved. The commit that moves it changes the
test and says why.

## Mechanism

Nothing here is a mechanism yet. The 128 MiB limit, `PROC_SHARED_MAP_MAX_PAGES`, is
enforced by `addrspace_charge_shared_map` alone and stays in force until the accounts
replace it as a set: [[sub-kernel-addrspace]] owns that axis, [[sub-kernel-pagemap]]
the capacity pool.

## Data structures

None of its own.

## Concurrency

None of its own. The tests run one at a time on the kernel test thread, and read
`capacity_pool_charged` before and after, as `test_capacity.c` does.

## Invariants enforced

None new. [[inv-i7]] says pages live until the last handle and the last mapping go;
the pins show who is charged while they do. [[inv-i32]] is the floor this design
replaces, not removes.

## Error paths

None of its own.

## Performance

Not applicable.

## Prosecution

No audit round has run on this note's tests: they change no kernel behaviour.

## Seams

The queued backend command, CPU composition borrow and display or GPU pin of the
design's section 6 have no kernel type: each reaches the kernel as a DMA handle or a
shared-in mapping held by a userspace service, lictor or tapestryd. Section 12 says
which, and what it has not yet read. The account, voucher
and pressure ABI layouts are not reserved, and no syscall number is taken.

## Caveats

A shared-in map cannot be cut, so a client always pays for the whole object it maps.
Only an owner's own DMA map placed inside the burrow window can be cut; outside the
window a hardware map detaches whole. So the partial-retention pin describes a
CAP_HW_CREATE holder, not an ordinary client.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
