---
id: sub-kernel-vma
type: sub
parent: moc-kernel-memory
title: "VMAs — the per-Proc address-space description, and where W^X is actually decided"
code: [kernel/vma.c, kernel/include/thylacine/vma.h]
audit: hard
guarded-by: [inv-i12, inv-i7, inv-i32, inv-i44]
validated-by: [prose, gate-smp]
locks: [lock-vma]
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md"]
created: 2026-08-03
updated: 2026-09-06
---
## Purpose

A VMA says "this range of user addresses is backed by that memory object, with
these permissions." The sorted list of them hanging off an address space *is* its
description — the thing a page fault is dispatched against, and the thing
`exec` builds when it lays out a binary.

It is a small file. It matters out of proportion to its size because
`vma_alloc` is where [[inv-i12]] is actually decided for userspace, and because
the list is the structure a fault handler walks while a sibling thread may be
tearing it down.

## Contract

`vma_alloc(start, end, prot, burrow, offset)` mints a descriptor and takes a
**mapping** reference on the Burrow. `vma_free` drops it. `vma_insert` links
into an address space's sorted list or rejects an overlap; `vma_remove` unlinks;
`vma_lookup` finds the VMA covering an address; `vma_find_gap` finds somewhere
to put a new one; `vma_drain` tears the whole list down at Proc death.

DISTRO D-3 added three operations for file-backed phenotype mmap.
`vma_replace_range_in` is the **MAP_FIXED split/replace** primitive (Mechanism,
below). `vma_next_overlap_in(lo, hi)` returns the lowest VMA overlapping a range
— the point-probe `vma_lookup` is blind to a VMA lying strictly inside one, which
the phenotype munmap row needs to tell "nothing mapped" from a partial overlap.
And `vma_free_deferred` is a `vma_free` that hands the caller the Burrow still
owing a physical free instead of freeing it inline — the discipline a
FILE-backed Burrow forces (Concurrency, below).

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

Overlap is **half-open**: `ranges_overlap(a, b, c, d) = a < d && c < b`, so two
ranges that merely touch at a boundary — `[a, b)` and `[b, c)` — are *adjacent*,
not overlapping, and both insert. This is the right rule for a page-aligned
address space where one mapping ends exactly where the next begins, and it is
what `vma.insert_overlap_rejected` pins against a partial-overlap regression.

`vma_find_gap` is written to never form the sum `cand + length`. Every
comparison is a subtraction guarded by an ordering test, so the arithmetic
cannot overflow for any window in the 47-bit user space. That is a deliberate
choice against a class of bug rather than a response to one.

**The MAP_FIXED split/replace** (`vma_replace_range_in`, D-3b) places a mapping
at a CHOSEN address, splitting whatever is there around it — the primitive musl's
`map_library` needs, which reserves a whole-span mapping and then overlays each
PT_LOAD onto a sub-range. Its domain is exactly two shapes: the range lies
**wholly inside** one existing VMA (split it into up to left / mid / right), or
the range is **entirely free** (a plain fixed-address insert). The free-space arm
is not optional — Linux MAP_FIXED does not require the target already mapped, and
refusing it answered `ENOMEM`, which an allocator cannot tell from real pressure
and reads as OOM (#196). Everything else — spanning two VMAs, partial overlap — is
refused, because partial unmap is post-v1.0 and musl's overlay never produces it.

Two properties carry the surgery. **No hole can exist on any failure path**,
because the old Vma is REUSED as the surviving remainder — shrunk in place
(`vaddr_end` for a left remainder; `vaddr_start` and `burrow_offset` together for
a right one) rather than removed — so a mid-insert failure restores three fields
and frees the un-inserted pieces, never having to re-insert a mapping it already
tore out. Only the exact-cover case removes the old VMA, and there the rollback
re-insert is provably infallible: same `as->lock` hold, into the range just
vacated (no overlap), with the VMA count strictly below its entry value (no cap
refusal). **The survivor keeps its `(burrow, offset)` relation EXACTLY** — for any
VA it still covers, `burrow_offset + (va - vaddr_start)` is invariant across the
cut — which is what lets the caller uninstall only the replaced window's PTEs and
leave the remainder's resident pages installed, and what makes the file-fault
arm's post-sleep geometry check (the #190 verify-and-bail) come out right against
a concurrent split: it passes exactly when the bytes read before the sleep still
belong at that slot. Allocation happens before any list mutation, and the I-32
headroom is checked before the mutation too, so neither a slab shortfall nor a cap
hit leaves partial state. A SHARED_IN, COW, or CODE-alias VMA is refused rather
than cut — none is reachable from the ldso overlay this serves, and the CODE
refusal is a parity guard against orphaning a JIT pair's peer (F8).

## Data structures

`struct Vma` is 64 bytes, pinned by `_Static_assert`, with `magic` at offset 0
— the SLUB freelist-clobber defence every long-lived kernel struct in this tree
carries. The fields: the half-open range, `prot`, `flags`, the backing Burrow
and byte offset into it, and the two list pointers.

`flags` was the alignment pad until G-2 needed `VMA_FLAG_SHARED_IN` — the
marker that this VMA's Burrow is *another Proc's* memory (a network flow ring
or a compositor weave) mapped in cross-Proc. It exists to make one accounting
statement exact: the shared-mapping budget must equal the summed pages
of flagged VMAs, so the flag is read at both teardown paths to uncharge exactly
once per charge.

The second flag marks a mapping as participating in **copy-on-write**, and three
things about it are worth stating because each would be natural to get wrong.

**It is routing, not truth.** The per-*page* share count is what decides an
individual break; the flag only says "a write here must go through the break
path rather than straight to the page". So a mapping whose pages have all been
taken in place costs one extra fault per page and nothing else.

**It is never cleared, deliberately.** Clearing it would require a scan proving
no page in the range is still shared — strictly more work than the extra faults
it would save, and a scan that could race the very sharing it is checking for.

**The permissions deliberately disagree with the hardware.** `prot` keeps its
write bit and the *page table entry* is what goes read-only. That inversion is
load-bearing: the fault handler's permission check reads `prot`, so a mapping
that dropped write would turn every copy-on-write write into a fatal fault
instead of a break. The entry is the enforcement; the mapping's permission is
the statement of what the program is *allowed* to do, and during copy-on-write
those are different questions.

That divergence does not touch [[inv-i12]], and the reason is worth being
explicit about rather than assumed: a copy-on-write mapping is writable and not
executable, so the rejected combination never arises. W^X is decided on the pair
at allocation, and nothing here can turn a write-only mapping into a
write-and-execute one afterward.

## Concurrency

**The list no longer hangs off the Proc.** Since the address-space extraction it
belongs to a `struct AddrSpace`, and the operations gained parallel forms that
take one directly — which is what lets `exec` populate a *detached* address space
it is still building, and what lets two Procs sharing one address space see one
list rather than two views of it. [[lock-vma]] moved with it.

**That lock serializes this list**, and every mutator holds it: the
attach and share paths, the detach and share-teardown paths, and — since G-3 —
`vma_drain` itself, which retired its lockless exemption when the weft reaper
gained a cross-Proc reclaim that holds a *target's* lock across a
multi-millisecond unmap loop. The reaper is why draining now takes a lock it is
otherwise uncontended on.

The move is more than a rename, and [[inv-i1]] is why it had to be one: two Procs
sharing an address space must contend on **the same** lock, not each on its own.
The old no-cycle argument was "a Proc never takes another Proc's list lock" —
disjointness. That argument is gone; what carries the property now is the
ordering, since two sharers take one lock rather than two.

The fault handler is a **reader** that holds the same lock, which is the whole
of the #713 fix: before it, an unlocked walker could follow a half-unlinked
list into a freed VMA and install a leaf PTE aliasing a page already recycled
into kernel memory.

**A FILE-backed Burrow's free may SLEEP, so it cannot happen under the lock
(D-3c F1/F5).** Freeing a 9P-backed FILE Burrow reaches `spoor_clunk`, which may
sleep — and every VMA mutator holds `as->lock`, a spinlock, so an inline free is
the lock-across-sleep extinction. `vma_free_deferred` splits the two halves: it
drops the mapping ref and settles the I-32 uncharge UNDER the lock (via
`burrow_release_mapping_deferred`, which does not free), but hands the caller the
Burrow that still owes a physical free, to pass to `burrow_free_deferred` AFTER
the unlock. `vma_drain_in` collects the dead Burrows on a `deferred_free_next`
stack and drains it past `spin_unlock`; `vma_replace_range_in`'s exact-cover arm
returns its one dead Burrow through a MANDATORY `out_free` (a NULL `out_free`
would LEAK it — the slab slot, the filepages, the pinned Spoor — which F7 judged
strictly worse than the inline-free-under-lock it replaced, so it fails loud).
The same hazard surfaced at a fourth site: F1 deferred the three teardown paths,
F5 caught the split's replace-free. It is latent today only because `/bin` execs
come from the non-sleeping devramfs; it becomes live for a 9P-paged exec text
Burrow (D-4/D-5).

**The header does not say this, and the gap has widened.** `vma_insert`'s
docblock still reads "Phase 5+ multi-thread Procs need a per-Proc lock around the
list; documented as a trip-hazard when added" — while `vma_find_gap`'s docblock,
twenty-five lines below it in the same file, instructs the caller to hold the lock
across the find and the insert. The `.c` is correct throughout; the `.h`
contradicts itself and is what a caller reads.

It is now wrong twice over: the lock exists, **and** it is no longer per-Proc.
A reader following that docblock would add a lock to the wrong structure, and the
resulting code would look right and serialize nothing between two Procs sharing
one address space. Task #60, and the extraction made it more expensive to leave.

## Invariants enforced

[[inv-i12]] — the `WRITE|EXEC` rejection, the sole gate through which every
user mapping in the system passes. The MAP_FIXED split/replace does not add a
second gate: it mints its new piece through `vma_alloc`, so the pair check runs
there, and it never mutates an existing VMA's `prot` (a split remainder keeps the
prot it already passed).

[[inv-i7]] — `vma_alloc` acquires a mapping reference, `vma_free` releases it.
The VMA's existence in a list *is* the mapping the Burrow's second refcount
counts. A guard VMA takes none, and `vma_free` is null-Burrow-safe, so the pair
stays balanced across both shapes.

**The release now reports whether it was the drop that freed the pages.** That
exists because "the mapping went away" and "the pages went away" are different
events the moment a region has a second owner — a ring, a registered buffer, a
cross-Proc share — and which drop is last cannot be predicted from the mapping's
type nor from a count sampled beforehand. Only the drop itself knows, so it
answers. The accounting sites pair their refund to that answer rather than to the
unmap.

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
`vma_find_gap` keeps its overflow-free arithmetic; that `vma_replace_range_in`
keeps every failure path hole-free (the old VMA reused as the survivor, never a
torn-out remap put back) and holds the survivor's `burrow_offset + (va -
vaddr_start)` invariant across the cut; that any path freeing a Burrow under
`as->lock` routes through the deferred free (a FILE Burrow's `spoor_clunk`
sleeps) and never drops the `out_free` it is handed; and that every new mutator
takes `vma_lock` — the header will not tell you to.

## Seams

- The header's stale lock commentary (task #60) is documentation, but it is the
  documentation a future multi-thread change would be read against.
- MAP_FIXED partial overlap (a range spanning two VMAs, or straddling one's
  boundary) is refused, not served — Linux would unmap the overlapped part, but
  partial unmap is post-v1.0. `vma_next_overlap_in` exists so the phenotype munmap
  row can tell a boundary-straddle (refused) from a wholly-unmapped range (a Linux
  success).
- An interval tree, if a workload ever puts enough VMAs on one Proc to matter.

## Caveats

`vma_lookup` takes an unaligned address deliberately — it is answering "which
VMA contains this faulting address", and a fault address is not page-aligned.
Callers wanting the page do the masking themselves.

## Provenance

P3-Da built the list; P6 #713 added the lock coverage that made it
multi-thread-safe; G-2 added the cross-Proc share flag; G-3 made `vma_drain`
take the lock. The I-32 charge arrived with the overcommit model.

Re-read 2026-08-16: the LINEAGE arc moved the list onto the address space and
added the copy-on-write flag; the attribution work made the release report
whether it freed. [[chg-2026-08-16-vma-cow-flag]].

DISTRO D-3 added the file-backed-mmap surface: D-3b the MAP_FIXED split/replace
(`vma_replace_range_in`), D-3c the FILE-backed VMA arm and the
sleeping-free-under-lock deferral (`vma_free_deferred`, F1/F5 — four sites) plus
`vma_next_overlap_in` (#199). [[chg-2026-09-06-vma-mapfixed-file-arm]].

## Tests

`kernel/test/test_vma.c` — six unit tests exercising this file directly:
`vma.alloc_free_smoke` (alloc/free; the `vma_total_allocated`/`_freed` counters
advance), `vma.alloc_constraints` (the rejections — zero-length, reversed,
unaligned, `WRITE|EXEC`, null Burrow — each return NULL),
`vma.insert_lookup_smoke` (three non-overlapping VMAs; lookup hits every covered
address and misses the gaps), `vma.insert_overlap_rejected` (exact and partial
overlaps return -1; an adjacent range touching at a boundary is accepted — the
half-open semantic), `vma.insert_sorted_invariant` (insert in mixed order, walk
ascending), and `vma.drain_releases_all` (insert four, drain, assert
`burrow_mapping_count` returns to baseline — the `vma_alloc` <-> `burrow_map`
symmetry). Beyond the suite, `vma_alloc`'s rejections and the list walk are
exercised indirectly by every demand-page and attach/detach test through the
fault path.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-fault]] · [[sub-kernel-mmu]] ·
[[sub-kernel-burrow]] · [[inv-i12]] · [[inv-i7]] · [[inv-i32]]
