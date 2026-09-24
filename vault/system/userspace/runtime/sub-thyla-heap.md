---
id: sub-thyla-heap
type: sub
title: "thyla-heap — the native heap: dlmalloc over reservations it owns, and a mapping of its own for a large block"
parent: moc-userspace-runtime
code:
  - usr/lib/thyla-heap/src/lib.rs
  - usr/lib/thyla-heap/src/tests.rs
  - usr/lib/thyla-heap/Cargo.toml
  - usr/heap-probe/src/main.rs
  - usr/heap-probe/Cargo.toml
audit: hard
guarded-by: [inv-i32]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 6.5"
created: 2026-09-24
updated: 2026-09-24
---
## Purpose

The heap under every native Thylacine program. libthyla-rs's `ThylaAlloc` is a
`Heap` over three burrow calls ([[sub-libthyla-rs]]), and this crate is the
policy: which memory a block comes from, when memory goes back, and what the
heap holds. It exists so the operator's memory bar holds for native code (ARCH
6.5 "Capacity"): a program is never refused memory the system has, and memory
it frees becomes the system's again. It replaced a fixed 4 MiB linked-list heap,
and the fixed 16 to 192 MiB variants six programs declared (B-1c, 2026-09-24;
`dec-2026-09-24-native-heap-large-blocks`).

## Contract

- `Heap<B>` implements `GlobalAlloc` over any `Backend`: `reserve(len,
  align_log2)` is a lazy, read-write, demand-zero reservation; `decommit(va,
  len)` returns the pages and leaves the range reserved, reading zero; `detach(va,
  len)` returns pages and address space. A refusal (`None`, `false`) changes
  nothing. libthyla-rs's backend is `SYS_BURROW_RESERVE` (124) at read-write,
  `SYS_BURROW_DECOMMIT` (84) and the range `SYS_BURROW_DETACH` (38); the host
  tests and the manual's bounds test supply their own.
- A block under 256 KiB (`DIRECT_MIN`) and aligned to less than 256 KiB comes
  from dlmalloc, which pads an alignment past a page inside a segment. Any other
  block is a reservation of its own, at its alignment, and is detached when
  freed.
- A refusal returns null. There is no fixed size and no initialization: the heap
  is a constant static, and nothing is reserved before the first allocation.
- `footprint()` is every byte the heap holds from the system: what dlmalloc has
  carved, in use or free, plus the direct blocks. A reader on any thread never
  sees less than the heap holds, and it bounds from above the data pages the
  kernel charges for the heap; the kernel's count of the program's pages adds
  the page tables and pagemap nodes that map them, about one page in 256. `peak()` is the most the footprint has been
  since `reset_peak()`, counted where it grows. `trim()` gives back what dlmalloc
  can now; `reservations()` counts dlmalloc's segments.
- `alloc_zeroed` of a direct block is a fresh reservation and is not written; a
  small one is dlmalloc's `calloc`, which clears it (no chunk here is mmapped, so
  dlmalloc never trusts the platform's zeroes).

## Mechanism

### dlmalloc and its platform

dlmalloc 0.2.14 (the copy the Rust standard library vendors, pinned at
`third_party/rust/dlmalloc`) asks its platform, the `Allocator` trait, for
segments, and gives memory back only two ways: `sys_trim` shrinks the top
segment through `free_part` once the top chunk passes 2 MiB (keeping less than
one 64 KiB granule), and `release_unused_segments` hands a wholly free segment
other than the newest to `free`. It runs on a free that leaves a top chunk past
the threshold, on an explicit trim, and every 4095 large frees. The port dropped
C dlmalloc's `mmap_alloc`, so its mmapped-chunk paths are unreachable and
`remap` is never called.

### Reservations: the platform owns the address space

ARCH read the platform literally at first: a fresh attach per `alloc`, a
decommit per `free_part`. That leaks. dlmalloc takes `free_part` to shrink the
segment for good, so the decommitted tail stays a live VMA dlmalloc has
forgotten, and the segment's later `free` releases only what dlmalloc still
counts, orphaning the tail: one VMA per trim-and-release cycle, toward
`PROC_VMA_MAX`, and so a refusal while memory is free.

`Reservations` carves every segment at a bump pointer inside a lazy reservation
it owns. The first is 256 MiB and each one opened while others are live doubles
it, to 32 TiB. `free_part` decommits the tail and moves the bump back over it,
so the next carve reuses that address space. `free` detaches the whole
reservation. A carve never fills its reservation; it needs strictly more room
than it asks for, so the reservation's last page is never carved. No new
reservation can then land at a segment's end, where dlmalloc would extend the
segment, and a first carve ends inside its own reservation, so it cannot meet a
segment's base, where dlmalloc would prepend. One reservation is one segment and
one VMA, and the segment dlmalloc trims is always the latest reservation's. A
length the kernel refuses is asked for again at half, down to the carve's own
size.

### Direct blocks

A block of 256 KiB or more, or one aligned to 256 KiB or more, is a reservation
of its own, detached on free. 256 KiB is C dlmalloc's `DEFAULT_MMAP_THRESHOLD`.
A smaller block with a large alignment stays dlmalloc's: a reservation each
would cost a mapping per block, and 65536 of them reach the per-program mapping
cap (`PROC_VMA_MAX`) while memory is free. Without
it every block lives in a segment, and a large buffer freed below a live block
keeps its pages. `GlobalAlloc` passes the layout, so a block carries no header.
`realloc` keeps a block whose page count is unchanged, detaches the tail of one
that shrinks, and moves one that grows (reserve, copy, detach). A resize across
the threshold allocates, copies and frees.

### Accounting

The carved bytes, the direct bytes and the peak sit in one table under the
heap's lock. A carve is counted in the same locked step that reserves it, and
trimmed or released bytes in the step that returns them. A direct block's
syscalls run outside the lock, so it is counted before its reservation is asked
for (and uncounted if refused) and uncounted only after its detach: at every
instant a reader can take the lock, the footprint is at least what is held. The
peak moves at the only two points the footprint grows: a carve, and a direct
block counted. A realloc that holds the old block and the new one for a moment
therefore counts both, and a refused direct block leaves the peak at what was
asked for -- an upper bound, never an undercount. A direct block that would take
the footprint past `isize::MAX` is refused before it is counted: no system holds
that much, and past it the counts could overflow.

## Data structures

- `Reservation { base, len, bump }`: dlmalloc's segment in it is `[base, bump)`.
- `Table { live: [Reservation; 64], n, carved, direct, peak }`, oldest first, in
  a `RefCell` inside `Reservations`, which dlmalloc owns inside the heap's lock.
- `Heap { small: Spinlock<Dlmalloc<Reservations<B>>>, backend: B }`, built by a
  `const fn`, so a program's heap is a plain static.

## Concurrency

One spinlock (spinning_top, the lock libthyla-rs's heap has always taken)
serializes dlmalloc and the accounting. A direct block's reserve and detach run
outside it and take it only to count the block, since a detach that frees tens
of megabytes of pages would otherwise stall every peer thread's small
allocations. The lock is a leaf: the backend never calls back into the heap. The
table's `RefCell` is borrowed only under the lock, because dlmalloc calls its
platform only while locked and every `Heap` accessor locks first.

## Invariants enforced

**[[inv-i32]]** — a reservation is lazy, so the heap's pages are charged to the
program's page budget as they are touched and uncharged as the heap decommits
or detaches them; the footprint follows what the program holds, both ways. The
reservation layer keeps the heap's VMAs at one per live reservation plus one per
live direct block, so the allocator's bookkeeping cannot eat the VMA axis
through repeated trims.

## Error paths

- A refused reservation: a carve asks again at half, down to its own size, then
  returns nothing, and dlmalloc's `malloc` returns null; a direct block returns
  null at once.
- A refused decommit: `free_part` returns false, dlmalloc keeps the segment's
  size, and the bump stays. The memory is kept, not lost.
- A refused detach: the reservation or direct block stays in the table and the
  count. The accounting stays true; only the mapping lingers.
- A full table (64 live reservations): the carve returns nothing.
- Null reaches libthyla-rs's default allocation-error path (a panic, exit 1)
  unless the caller used a fallible form.

## Performance

Growth inside a reservation costs no syscall; dlmalloc's 2 MiB trim threshold is
the hysteresis between giving back and churning. Measured at boot by
`/heap-probe` (QEMU, -smp 4, 2026-09-24), in nanoseconds per page:
allocating and first touching small blocks 1630, freeing them (trim and
decommit included) 351, touching a fresh 32 MiB direct block 1062, detaching it
344. The eager release's churn (B-1b's F4) is the gap between a heap that swings
4 MiB past the threshold, 1070 per page per cycle (decommitted and faulted in
again), and one that swings 1.5 MiB under it, 48.

## Prosecution

The audit-trigger row is "The native heap (thyla-heap + ThylaAlloc)" in
`docs/AUDIT-TRIGGERS.md`. Its categories: a decommit reaching a live chunk (the
bump rollback against dlmalloc's own trim arithmetic), segment merging (a carve
that fills its reservation or a reservation landing against a segment),
accounting drift between the footprint and the kernel's census, the realloc
paths across the threshold, alignment past a page, and overflow in the length
arithmetic. Host tests (twelve, `cargo test -p thyla-heap --target
aarch64-apple-darwin`) run the real dlmalloc over the real `Reservations` with a
first-fit model of the kernel that poisons every page it takes back; thirteen
platform sabotages each turn them red, three of them the direct block's counting
order (a backend that reads the footprint, as a peer thread would, at each
reserve and detach), one the alignment routing and one the footprint's bound.
`/heap-probe` is the guest half: the census rises past 64 MiB of small blocks
and falls to within 13 pages of its base without a trim being asked for; a
trim returns the free tail the frees kept, after the leg has checked that tail
is still held (243 pages; with the trim a no-op the leg fails by name); a
patterned block below each trim stays intact; a 32 MiB block returns every page
while a small one stays live; and a reservation switch releases the emptied
first reservation at the free that empties it, before any trim is asked for.

## Seams

- The manual's bounds test (`usr/manual/src/bounds.rs`, [[sub-manual]]) runs
  the reader on this heap over an arena standing in for the kernel, and bounds
  its peak footprint (MANUAL-DESIGN 8.1).
- Pouch programs do not use this heap: musl's mallocng returns their memory
  through `madvise` and `munmap` (B-1b).
- corvus declares its own static-BSS allocator and does not link this one.

## Caveats

- **Small blocks freed below a live one keep their pages.** dlmalloc returns
  memory only from the top of the newest segment and from a segment it has
  wholly emptied. A run of small blocks freed beneath one that stays live is
  reused by later allocations, not returned: dlmalloc's documented behaviour, and
  glibc's main arena's. Direct blocks are exempt.
- **A heap past 256 MiB keeps its older segments while anything in them lives.**
  Crossing a reservation opens a larger one; the older is released only once
  every block in it is freed.
- **The table's 64 slots.** Seventeen doubled reservations reach half the burrow
  window. Past them, the table holds the smaller reservations a full or
  fragmented window still grants, so it fills only once the window is all but
  exhausted.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
