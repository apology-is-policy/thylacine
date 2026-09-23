---
id: sub-kernel-vma
type: sub
parent: moc-kernel-memory
title: "VMAs — the per-Proc address-space description, and where W^X is actually decided"
code: [kernel/vma.c, kernel/include/thylacine/vma.h]
audit: hard
guarded-by: [inv-i12, inv-i7, inv-i32, inv-i44]
validated-by: [spec-cow, prose, gate-smp]
locks: [lock-vma]
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md"]
created: 2026-08-03
updated: 2026-09-23
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

B-1a (2026-09-23) added the **permission ceiling** and the **multi-mapping
reprotect**. `vma_prot_max(v)` / `vma_set_prot_max(v, prot)` read and write the
ceiling held in `flags` bits 8..10 (`VMA_FLAG_MAX_SHIFT` / `VMA_FLAG_MAX_MASK`);
`vma_alloc` sets it to the mint prot. `vma_reprotect_precheck_in(as, vaddr,
length, prot)` decides EVERY refusal of a protect over `[vaddr, vaddr+length)`
without mutating anything, and `vma_reprotect_range_in(as, exempt, vaddr,
length, prot, seal)` runs the precheck again, cuts the first and last mapping
where the range crosses them, changes every interior mapping in place, and
merges equal neighbours (Mechanism, below). `vma_find_gap_aligned(p, length,
align, window_start, window_end, out)` is `vma_find_gap` with the candidate
rounded up to a power-of-two `align` (`vma_find_gap` is now the `align = 0`
wrapper). `VMA_FLAG_STATE_MASK` names the two STATE bits (`SHARED_IN | COW`)
so the D-3b split's "no flags" refusal reads only those, and its right piece
keeps the ceiling.

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

**The permission ceiling** (B-1a; ARCH 6.5). Every VMA carries, in `flags`
bits 8..10, the `VMA_PROT_*` set its `prot` may never be raised past. The
ceiling is fixed at the mint and only ever lowered (`seal`); nothing raises it.
`vma_alloc` sets it to the MINT prot, which is the safe default rather than the
lazy one: the vDSO clock page is a kernel-owned eager anon Burrow mapped
read-only into every address space (`exec_map_vdso`), and a default of RW would
let any Proc raise that mapping and write the kernel's clock. The mints that
reserve now and commit later -- `SYS_BURROW_RESERVE`, the phenotype anon-mmap
arm, the fixed-anon arm -- raise the ceiling to RW explicitly under the same
lock hold, before anything can observe the mapping
([[sub-kernel-syscall-dispatch]]). A guard's ceiling is none. A split piece
carries its parent's; a fork's child carries the parent's
([[sub-kernel-addrspace]]).

**The multi-mapping reprotect** (`vma_reprotect_*`, B-1a). A protect moves
every page of a range to a `prot` in {none, R, RW}. **X is never a target**,
refused twice: at the syscall boundary before any lookup
(`burrow_prot_word_check`, [[sub-kernel-syscall-dispatch]]) and again in
`vma_reprotect_precheck_in`, so no in-kernel caller can reach an RX target by a
path the boundary did not see. The precheck is the whole refusal set, decided
BEFORE the first mutation: a malformed range or word (`-T_E_INVAL`); a hole, a
guard, or no room for the split pieces (`-T_E_NOMEM`); a shared-in mapping, a
CODE alias, a hardware mapping (MMIO / DMA / HOSTMEM -- `reprotect_admits`
admits only ANON, ANON_LAZY and FILE), or a `prot` above the ceiling
(`-T_E_ACCES`). A refusal leaves every mapping exactly as it was, which is
STRONGER than Linux, where a failed `mprotect` may have changed part of the
range.

The range may span SEVERAL mappings, all-or-nothing. This is the one place the
build departed from the ratified letter ("within ONE mapping; refused at v1, no
producer"), and ARCH 6.5 is amended AS BUILT with the reasoning: the merge pass
makes an engine's grow / shrink ladder over a reservation exactly two mappings
-- the committed prefix and the `none` tail -- so a whole-region protect over a
partially committed reservation IS a two-mapping range, and Linux serves it.
Only the first and the last mapping can be cut, so at most two new pieces
exist, allocated BEFORE the list is touched (the D-3b shape) and each carrying
its parent's `flags` whole -- the ceiling AND the COW routing bit, since the
per-page share counts are per page and a cut is sound for a forked mapping. The
I-32 headroom for those pieces is checked before the mutation, so a cap hit
changes nothing. The ORIGINAL structs survive as the in-range pieces, shrunk in
place (`vaddr_start` and `burrow_offset` move by the same delta on a left cut;
only `vaddr_end` on a right cut), so an interior mapping is never reallocated
and no mapping ref ever drops -- the MAP_FIXED split's identity rule again: for
every VA a piece still covers, `burrow_offset + (va - vaddr_start)` is
unchanged. Then the APPLY loop sets `v->prot = prot` (and the ceiling too, on
`seal`) on every overlapping VMA, and `reprotect_merge_in` coalesces each pair
that involves an affected piece -- the left neighbour with the first piece, the
pieces among themselves, the last piece with the right neighbour -- wherever
`reprotect_mergeable` holds: same Burrow, adjacent, same `prot` and `flags`
(so the same ceiling and the same COW bit), contiguous `burrow_offset`, and
never a `SHARED_IN` mapping (its exact geometry is what the sharer's detach
matches). The merge drops the absorbed VMA through `vma_free_deferred(b,
NULL)`; its neighbour still maps the same Burrow, so that drop can never be
the last, and the "owes a free" return is an extinction rather than a leak.

**The PTE uninstall is the caller's half of the contract, and it runs FIRST.**
`burrow_protect_in` ([[sub-kernel-burrow]]) is precheck ->
`mmu_uninstall_user_range` over the range -> `vma_reprotect_range_in`, one
locked step. The order is the D-3b rule and `addrspace_clone`'s phase-1
argument: hardware resolves a PTE without taking `as->lock`, so a writable PTE
must be gone before the prot that justified it is
(`cow.tla::BUGGY_PROTECT_KEEPS_PTE`). The uninstall runs on a raise too,
because `mmu_install_user_pte` refuses a mismatching install over a valid leaf
-- the next fault after a raise must find the slot empty so it can install at
the NEW prot ([[sub-kernel-fault]] step 2: refused at none, read-only at R,
writable at RW). A resident page costs one re-fault and nothing else: its slot
and its charge stay, as Linux keeps a PROT_NONE mapping's contents;
`SYS_BURROW_DECOMMIT` is how pages are returned.

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

**Bits 8..10 of `flags` are the permission ceiling** (B-1a), read by
`vma_prot_max` and written by `vma_set_prot_max` -- placed in `flags` so the
struct stays at its pinned 64 bytes. Everything below bit 8 is the mapping's
STATE (`VMA_FLAG_STATE_MASK` = `SHARED_IN | COW`), and the two are kept apart
so that a "has no flags" test (the D-3b split's refusal) reads the state bits
only and does not refuse every mapping whose ceiling is non-zero -- which is
all of them.

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
write-and-execute one afterward. B-1a's reprotect keeps that true by
construction rather than by a second check: its targets are {none, R, RW}, so
no protect can add EXEC to anything (Invariants, below).

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

**`vma_free`'s teardown clears the PTEs before it frees the pages, and that
order is the corruption boundary.** It calls `mmu_uninstall_user_range` over the
VMA's range — clearing the leaf PTEs and broadcasting `tlbi vaae1is` — *before*
the backing pages return to the buddy. Freeing first would leave live PTEs and
cached TLB entries pointing at pages the allocator has since handed to someone
else: the stale-mapping class suspected behind the AEGIS-256/mallocng
corruption. The clear is idempotent on never-faulted-in pages, so it runs
unconditionally over the range.

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

**A protect holds `as->lock` across the precheck, the uninstall and the cut,
and takes no other lock.** `mmu_uninstall_user_range` allocates nothing and
cannot fail; the two pieces are allocated before the list is touched; the merge
drops through `vma_free_deferred`, whose drop can never be a Burrow's last here.
So nothing under the hold can sleep and nothing after the uninstall can fail
except a piece allocation, which leaves every `prot` unchanged. A peer thread
storing through an already-installed writable PTE is the one actor the lock
cannot reach, and the uninstall-first order is what reaches it.

## Invariants enforced

[[inv-i12]] — the `WRITE|EXEC` rejection, the sole gate through which every
user mapping in the system passes. The MAP_FIXED split/replace does not add a
second gate: it mints its new piece through `vma_alloc`, so the pair check runs
there, and it never mutates an existing VMA's `prot` (a split remainder keeps the
prot it already passed). B-1a's reprotect is the SECOND path that writes a
VMA's `prot`, and it adds no second W^X gate for the same reason: EXEC is
outside its target set entirely, refused by `vma_reprotect_precheck_in` before
any other test (and by the syscall boundary before that). The ceiling adds a
third statement, stronger than the gate: an RX mapping -- image text -- can
only descend to R or none and never returns, because nothing raises a
ceiling and a raise back to X is refused regardless. So "no page W and X" still
has exactly one place where W and X are decided together (`vma_alloc`), and the
one way bytes a Proc wrote become executable is still `CAP_JIT`'s dual map.

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
pages, so without a VMA cap a Proc could exhaust the descriptor slab. The
reprotect keeps the same shape: `cnt > PROC_VMA_MAX - adding` is checked
before the mutation for the (at most two) pieces a cut adds, the inserts then
charge as usual, and the merge uncharges through `vma_remove_in`. The merge is
also what keeps the count meaningful under an engine's ladder: `PROC_VMA_MAX`
is 65536, and a 4 GiB Wasm reservation grown in 64 KiB pages would reach it
without the coalesce (`protect.grow_ladder_stays_two_vmas`).

[[inv-i44]] — a cut on a COW mapping copies the COW bit and the ceiling to
the pieces, and the per-page share counts are per page, so the pieces break
independently and soundly (`protect.cow_split_then_break`). The fork's side of
that -- one clone per source Burrow rather than one per piece -- is
[[sub-kernel-addrspace]]'s, and the reason it had to change is this file's
split.

## Error paths

Every rejection is a `NULL` or `-1` return with nothing allocated and nothing
linked — there is no partial state to unwind. The extinctions are reserved for
conditions that mean memory is already corrupt: a bad magic, freeing a VMA
still in a list, inserting one already linked, or finding a corrupted entry
mid-walk. Those are not error handling; they are the structure declaring it can
no longer be trusted.

The reprotect family is the first in this file to speak errno rather than
`-1`: `-T_E_INVAL` / `-T_E_NOMEM` / `-T_E_ACCES`, the table in `vma.h`.
Its callers are the two errno-returning syscalls and the phenotype `mprotect`
row, which hands the values to a Linux guest as Linux's own.

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

Since B-1a, five more: that `vma_reprotect_precheck_in` refuses exactly the
set `vma_reprotect_range_in` cannot handle (the second runs the first, so a
precheck that admits what the cut cannot take is the hazard); that EXEC stays
outside the target set at BOTH refusal sites; that the cut copies `flags`
WHOLE -- a piece that dropped the COW bit would route a write on a shared page
straight to the page (an [[inv-i44]] alias), and one that dropped the ceiling
would default a reserve minted at none to un-raisable; that `reprotect_mergeable`
compares `flags` whole (merging a sealed piece with an unsealed neighbour would
give the merged mapping one ceiling for both); and that the uninstall in
`burrow_protect_in` stays ahead of the first `prot` write, on a raise as much
as on a lowering.

A separate rule governs the geometry-matching *removers* rather than this file's
own arithmetic: **`vma_remove` / `burrow_unmap` match a VMA by its coordinates
alone, so any syscall that hands them a user-supplied `(vaddr, length)` must decide
that the caller is entitled to unmap THAT VMA before the match runs.**
`SYS_BURROW_DETACH` decides it by **identity** (ARCH 6.5, operator-ratified
2026-09-16), in `kernel/syscall.c`'s three halves:
- `detach_shape_check`: a non-zero length, a page-aligned base, and a span
  inside `USER_VA_TOP`.
- `detach_in_window`: the burrow-attach window `[EXEC_USER_BURROW_BASE,
  EXEC_USER_BURROW_TOP)`, the P6-pouch-mem-a F1 bound. It is still the whole
  rule for the phenotype `munmap` range, which keeps `detach_args_check`.
- `detach_is_hw_map_locked`: outside the window, ONLY a VMA backed by a DMA or
  MMIO Burrow, read under the same `as->lock` hold that removes it, with exact
  geometry.

Without a rule, a caller could pass the coordinates of its own ELF-segment,
stack or stack-guard VMA and have it dismantled; the stack-guard case silently
retires a security-relevant page. None of those is ever DMA- or MMIO-backed:
- ELF segments are FILE or ANON;
- the stack is ANON;
- the guard is a Burrow-less VMA (`vma_alloc_guard`);
- the vDSO is a kernel ANON Burrow;
- a CODE alias is refused on its own.

The two hardware types reach an address space only through `SYS_DMA_MAP` /
`SYS_MMIO_MAP` / `SYS_PCI_MAP_BAR`, or through the weft share, which places
inside the window. `addrspace_clone` refuses to fork them.

**Why the window alone was wrong.** It protected by LOCATION, a proxy that also
covered every hardware map a driver placed below 4 GiB, so `SYS_DMA_MAP` accepted
placements `SYS_BURROW_DETACH` could never remove. tapestryd mapped every weave,
GPU buffer and ring at `0x0240_0000`+ from G-3a on, and every release returned -1
into a discarded result. A kernel probe measured it: objects created minus objects
live held at exactly 46 across a whole session while 415 accumulated, until a weave
create failed mid-drag ([[sub-tapestryd]]). The identity form is Plan 9's:
`syssegdetach` refuses the initial stack by `s == up->seg[SSEG]` and detaches any
other segment, device segments at caller-chosen addresses included.

**The lazy-piece refund (B-1a, the first of three findings the split made
live).** Once a lazy Burrow has several VMAs -- a D-3b window, or the pieces a
protect leaves -- the detach path's refund could no longer be the WHOLE
Burrow's resident count per piece: that under-counted I-32 once per piece.
`detach_one_locked` now refunds the piece's own range through `burrow_decommit`
on the exact match, before `burrow_unmap_reporting`
([[sub-kernel-syscall-dispatch]]), which also returns a detached piece's pages
to the system at once. The native detach itself stays exact-match per VMA: the
pieces a protect leaves detach one by one (`/protect-probe` does exactly that),
and the range form is B-1a'.

## Seams

- The header's stale lock commentary (task #60) is documentation, but it is the
  documentation a future multi-thread change would be read against.
- MAP_FIXED partial overlap (a range spanning two VMAs, or straddling one's
  boundary) is refused, not served — Linux would unmap the overlapped part, but
  partial unmap is post-v1.0. `vma_next_overlap_in` exists so the phenotype munmap
  row can tell a boundary-straddle (refused) from a wholly-unmapped range (a Linux
  success).
- An interval tree, if a workload ever puts enough VMAs on one Proc to matter.
- The native `SYS_BURROW_DETACH` matches one VMA exactly while a protect can
  leave several pieces, and a sealed piece can never merge back into an
  unsealed neighbour; the range detach is B-1a' ([[arc-boosty]]). The phenotype
  `munmap`'s window confinement -- a `MAP_FIXED` mapping below the window leaks
  -- is owned there too ([[sub-kernel-syscall-dispatch]]).

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

[[chg-2026-09-06-sys-burrow-doc-absorb]] folds the P6-pouch-mem-a F1 finding
absorbed from docs/reference/79: `SYS_BURROW_DETACH` must bound its user-supplied
vaddr to the burrow-attach window before `burrow_unmap`, because the remover
matches by geometry alone — else EL0 dismantles its own ELF/stack/stack-guard VMA
(the guard case silently retiring a security page).

2026-09-16: the detach admission became identity-scoped (ARCH 6.5; scripture
`0fbeaf3c`). The window bound stays; a DMA- or MMIO-backed VMA is admitted outside
it. Tests `sys_burrow.detach_dma_map_by_identity` and
`sys_burrow.detach_mmio_map_by_identity` (a >256 MiB BAR-shaped map; the claim is
released) fail on the window-only rule. `sys_burrow.detach_window_confined` now
also installs a guard VMA, and is the control that an identity arm admitting ANON
fails. All three were sabotage-measured.

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

`kernel/test/test_protect.c` (B-1a; [[sub-kernel-protect-witness]]) drives the
reprotect through the `_for_proc` inners on fresh Procs: `protect.reserve_mints_exactly`,
`reserve_refusals`, `raise_and_write_keeps_contents`, `x_refused_before_lookup`
(EACCES over an unmapped range where R answers ENOMEM), `ceiling_bounds_raise`
(an eager mapping minted R cannot be raised; RX descends and never returns),
`seal_lowers_ceiling`, `split_three_way_then_merge` (three pieces of one
Burrow with every surviving byte's identity unchanged, merged back into ONE;
pages and Vma structs return to their counts), `grow_ladder_stays_two_vmas`,
`refusals_change_nothing` (a guard, a shared-in mapping, a CODE alias, an
unaligned address, unknown flags, a zero length -- each its errno, the mapping
byte-identical after), `multi_vma_and_hole` (two Burrows both change and do not
merge; a leading, interior or trailing hole changes nothing),
`pte_uninstalled_then_reinstalled_at_prot`, and `cow_split_then_break`. Four
sabotages were measured against them (the uninstall skipped, both X refusals
dropped, the ceiling compare skipped, a clone per VMA), each failing exactly its
own assertions and nothing else.

The holotype audit's regressions (the close commit): `protect.range_walk_is_linear`
(1024 adjacent mappings; the protect's and the phenotype munmap's scans stay
under 64 x k by the `vma_scan_steps` counter, where a re-scan per mapping cost
k(k+1)/2 per pass), `protect.uninstall_range_skips_absent_subtrees`, and
`protect.noop_protect_needs_no_headroom` (at `vma_count = PROC_VMA_MAX - 1` a
same-prot protect answers 0 and uninstalls nothing; the same sub-range at a
new prot is the ENOMEM control). A combined RED (every fix reverted, the
diagnostics and tests kept) fails exactly the six new tests.

## The holotype audit's corrections (2026-09-23)

**Four quadratic passes under a non-preemptible lock (F2, P2).**
`vma_next_overlap_in` restarts from the LIST HEAD on every call, and the
precheck, the first/last scan and the apply loop each iterated a range with it
-- Sigma(i + j) ~ k x i + k^2 / 2 node visits per pass over k mappings at
list position i, four passes per protect, all under `as->lock`, whose holder
is non-preemptible (`spinlock.h`). An unprivileged Proc can make k = 65535
adjacent single-page reserves and protect them in one call: ~8.6e9 dependent
pointer chases, seconds to tens of seconds with a CPU held and every sibling
that needs the lock spinning non-preemptibly too. The phenotype `munmap`'s
two loops had the same shape ([[sub-kernel-syscall-dispatch]]). Every range
walk is now ONE scan from the head and then `v = v->next` while
`v->vaddr_start < end` -- the list is sorted and the precheck has proven
contiguity -- with the magic check kept per node. `vma_scan_steps()` counts
the nodes `vma_next_overlap_in` visits, so the bound is pinned by a count,
never by the clock. The uninstall leg of the same finding is
[[sub-kernel-mmu]]'s.

**A no-op protect needed headroom (F6, P3).** A sub-range protect to the prot
it already had still cut two pieces (the cut is geometric) and demanded their
I-32 slots before the merge folded them back, so at `PROC_VMA_MAX - 1` a
no-op answered ENOMEM where Linux succeeds. `vma_reprotect_is_noop_in` (every
mapping already at `prot`; with `seal`, every ceiling too) is consulted by
`burrow_protect_in` before the uninstall and by `vma_reprotect_range_in`
before the cut; a no-op costs the range nothing, not even a re-fault. The
same test's control -- "the ENOMEM refusal changed nothing" -- was false on
its first run, because the I-32 headroom for the pieces was decided in
`vma_reprotect_range_in`, AFTER `burrow_protect_in`'s uninstall, so a cap hit
refused with the range's PTEs already gone (the documented re-fault cost).
`vma_reprotect_headroom_in` now decides it in `burrow_protect_in` after the
no-op short-circuit and before the uninstall; `vma_reprotect_range_in` keeps
its own check under the same lock hold. The only refusal that can follow the
uninstall is a slab shortfall for a piece.

## Referenced by

[[moc-kernel-memory]] · [[sub-kernel-fault]] · [[sub-kernel-mmu]] ·
[[sub-kernel-burrow]] · [[inv-i12]] · [[inv-i7]] · [[inv-i32]]
