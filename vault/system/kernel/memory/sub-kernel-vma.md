---
id: sub-kernel-vma
type: sub
parent: moc-kernel-memory
title: "VMAs — the per-Proc address-space description, and where W^X is actually decided"
code: [kernel/vma.c, kernel/include/thylacine/vma.h]
audit: hard
guarded-by: [inv-i12, inv-i7, inv-i32]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

A VMA says "this range of user addresses is backed by that memory object, with
these permissions." The sorted list of them hanging off a Proc *is* its address
space description — the thing a page fault is dispatched against, and the thing
`exec` builds when it lays out a binary.

It is a small file. It matters out of proportion to its size because
`vma_alloc` is where [[inv-i12]] is actually decided for userspace, and because
the list is the structure a fault handler walks while a sibling thread may be
tearing it down.

## Contract

`vma_alloc(start, end, prot, burrow, offset)` mints a descriptor and takes a
**mapping** reference on the Burrow. `vma_free` drops it. `vma_insert` links
into a Proc's sorted list or rejects an overlap; `vma_remove` unlinks;
`vma_lookup` finds the VMA covering an address; `vma_find_gap` finds somewhere
to put a new one; `vma_drain` tears the whole list down at Proc death.

**Four rejections at `vma_alloc`**, and the order they are written in is not
the order they matter in:

| rejected | why |
|---|---|
| `WRITE \| EXEC` | [[inv-i12]] — **the single user-side W^X gate** |
| `WRITE` without `READ` | AArch64 has no write-only AP encoding, so a W-only request would silently map *readable*. Rejecting keeps the VMA's prot and the PTE's meaning identical |
| `start >= end`, misalignment | a range that is not a range |
| null Burrow | a mapping with nothing behind it (except a guard — below) |

A **guard VMA** is the deliberate exception to the last: `prot == 0`, no
Burrow, existing only to occupy address space. Two things follow from prot 0
and both are wanted — `vma_insert`'s overlap rejection keeps anything else out
of the range, and the fault handler's permission check fails for read, write
*and* instruction alike, so it returns before it can dereference the null
Burrow. That is the unmapped page below the user stack: an overflow faults
rather than quietly landing in whatever is mapped beneath.

## Mechanism

A sorted doubly-linked list, ascending by start address. O(N) everything, which
is correct at this scale — a Proc has a handful of segments and a stack, and an
interval tree would be more machinery than the problem has.

The sortedness buys three things beyond insertion order. `vma_lookup` can stop
early: once a node starts above the target, no later node can contain it.
`vma_insert` finds its slot and detects overlap in the same single pass.
`vma_find_gap` is a single forward sweep carrying a candidate base that jumps
past each blocking VMA — first-fit, lowest gap, one pass.

`vma_find_gap` is written to never form the sum `cand + length`. Every
comparison is a subtraction guarded by an ordering test, so the arithmetic
cannot overflow for any window in the 47-bit user space. That is a deliberate
choice against a class of bug rather than a response to one.

## Data structures

`struct Vma` is 64 bytes, pinned by `_Static_assert`, with `magic` at offset 0
— the SLUB freelist-clobber defence every long-lived kernel struct in this tree
carries. The fields: the half-open range, `prot`, `flags`, the backing Burrow
and byte offset into it, and the two list pointers.

`flags` was the alignment pad until G-2 needed `VMA_FLAG_SHARED_IN` — the
marker that this VMA's Burrow is *another Proc's* memory (a network flow ring
or a compositor weave) mapped in cross-Proc. It exists to make one accounting
statement exact: the per-Proc shared-mapping budget must equal the summed pages
of flagged VMAs, so the flag is read at both teardown paths to uncharge exactly
once per charge.

## Concurrency

**`Proc.vma_lock` serializes this list**, and every mutator holds it: the
attach and share paths, the detach and share-teardown paths, and — since G-3 —
`vma_drain` itself, which retired its lockless exemption when the weft reaper
gained a cross-Proc reclaim that holds a *target's* `vma_lock` across a
multi-millisecond unmap loop. The reaper is why draining now takes a lock it is
otherwise uncontended on.

The fault handler is a **reader** that holds the same lock, which is the whole
of the #713 fix: before it, an unlocked walker could follow a half-unlinked
list into a freed VMA and install a leaf PTE aliasing a page already recycled
into kernel memory.

**The header does not say this.** `vma_insert`'s docblock still reads "Phase 5+
multi-thread Procs need a per-Proc lock around the list; documented as a
trip-hazard when added" — while `vma_find_gap`'s docblock, twenty-five lines
below it in the same file, instructs the caller to hold `Proc.vma_lock` across
the find and the insert. The `.c` is correct throughout; the `.h` contradicts
itself and is what a caller reads. Task #60.

## Invariants enforced

[[inv-i12]] — the `WRITE|EXEC` rejection, the sole gate through which every
user mapping in the system passes.

[[inv-i7]] — `vma_alloc` acquires a mapping reference, `vma_free` releases it.
The VMA's existence in a list *is* the mapping the Burrow's second refcount
counts. A guard VMA takes none, and `vma_free` is null-Burrow-safe, so the pair
stays balanced across both shapes.

[[inv-i32]] — the live-VMA count is charged at insert and uncharged at remove.
The charge sits deliberately **after** the overlap walk (a rejected overlap
must not consume budget) and **before** the list mutation (a cap hit must
install nothing), so neither failure path needs a rollback. It bounds the DoS a
free lazy reservation would otherwise open: the reservation itself costs no
pages, so without a VMA cap a Proc could exhaust the descriptor slab.

## Error paths

Every rejection is a `NULL` or `-1` return with nothing allocated and nothing
linked — there is no partial state to unwind. The extinctions are reserved for
conditions that mean memory is already corrupt: a bad magic, freeing a VMA
still in a list, inserting one already linked, or finding a corrupted entry
mid-walk. Those are not error handling; they are the structure declaring it can
no longer be trusted.

## Performance

O(N) per operation against an N of a handful. The tradeoff is stated in the
header and remains right: an interval tree pays its complexity back only past
roughly thirty entries per Proc, which nothing here reaches.

## Prosecution

The things to re-examine when this file changes: that `vma_alloc` remains the
only way a user mapping is born (the moment a second path exists, [[inv-i12]]
has two gates and one of them will drift); that the charge/uncharge pairing
stays exact across every path including the flagged cross-Proc shape; that
`vma_find_gap` keeps its overflow-free arithmetic; and that every new mutator
takes `vma_lock` — the header will not tell you to.

## Seams

- The header's stale lock commentary (task #60) is documentation, but it is the
  documentation a future multi-thread change would be read against.
- An interval tree, if a workload ever puts enough VMAs on one Proc to matter.

## Caveats

`vma_lookup` takes an unaligned address deliberately — it is answering "which
VMA contains this faulting address", and a fault address is not page-aligned.
Callers wanting the page do the masking themselves.

## Provenance

P3-Da built the list; P6 #713 added the lock coverage that made it
multi-thread-safe; G-2 added the cross-Proc share flag; G-3 made `vma_drain`
take the lock. The I-32 charge arrived with the overcommit model.

## Tests

Driven indirectly by every demand-page and attach/detach test — `vma_alloc`'s
rejections are exercised through `burrow_map`, and the list operations through
the fault path. There is no dedicated `vma.*` suite; the structure is proven by
its users.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-fault]] · [[sub-kernel-mmu]] ·
[[sub-kernel-burrow]] · [[inv-i12]] · [[inv-i7]] · [[inv-i32]]
