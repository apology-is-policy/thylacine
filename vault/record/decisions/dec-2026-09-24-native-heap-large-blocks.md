---
id: dec-2026-09-24-native-heap-large-blocks
type: dec
title: "B-1c: the native heap's reservations and its direct-mapped large blocks"
date: 2026-09-24
status: standing
decided-by: user-vote
affects: [inv-i32, sub-libthyla-rs, sub-manual]
created: 2026-09-24
---
## Fork

B-1c replaces libthyla-rs's fixed 4 MiB heap with `dlmalloc-rs` over a
Thylacine platform trait, as the B-1 vote set it
([[dec-2026-09-23-memory-surface-and-loader]], row 9) and ARCH 6.5 reads:
`alloc` = lazy attach, `free` = detach, `free_part` = decommit. Designing it
from the crate's source -- dlmalloc 0.2.14, the copy Rust's own standard
library vendors -- raised two questions scripture did not settle. Both were put
to the operator by blocking question on 2026-09-24 (Opus 5.5, under the away
grant's Opus clause): whether ThylaAlloc should map large blocks directly, since
the Rust port has no large-block path; and what MANUAL-DESIGN 8.1's bounds test
should measure once `ThylaAllocN`, the allocator it names, goes with the fixed
heap. A third finding needed no vote because scripture already answers it: the
literal mapping is sound only if the platform owns the address space it hands
out.

## Research

- dlmalloc 0.2.14's `sys_alloc` (dlmalloc.rs:440) only makes or extends
  segments. C dlmalloc's `mmap_alloc` (DEFAULT_MMAP_THRESHOLD 256 KiB) was not
  ported; the mmapped-chunk free and resize paths that remain are unreachable.
  Memory goes back only through `sys_trim` (the top segment's tail, through
  `free_part`, once the top chunk passes 2 MiB) and `release_unused_segments`
  (wholly free non-head segments). The pages of a free chunk below a live one
  are never returned.
- Heritage and SOTA: C dlmalloc, glibc (128 KiB, dynamic to 32 MiB) and musl
  mallocng (about 128 KiB) map large blocks directly and unmap them on free.
  Plan 9's pool allocator grows by `segbrk` and never shrinks, so it has no
  answer to offer here.
- Tree facts: `SYS_BURROW_ATTACH_LAZY` charges a VMA slot at attach and pages at
  touch (syscall.h:1612); `PROC_VMA_MAX` is 65536 (proc.h:152); `vma_find_gap`
  is a first-fit list walk (vma.c:542); `SYS_BURROW_DECOMMIT` frees and
  uncharges pages but keeps the VMA, and Go's `sysUnused` already rides it
  (syscall.h:1623); `SYS_BURROW_RESERVE` takes an alignment (syscall.h:2319).
- The literal mapping leaks. With a fresh attach per `alloc` and `free_part` =
  decommit, the decommitted tail stays as a live VMA that dlmalloc has
  forgotten, and the later `free(base, newsize)` orphans it. That is one leaked
  VMA per trim-and-release cycle, heading toward the cap: a refusal while free
  memory exists.

## Options

1. Large blocks. (a) Direct-map at 256 KiB and above, C dlmalloc's own
   threshold. (b) Pure dlmalloc-rs as ARCH read, with the pinned-block case
   recorded as a caveat against the bar.
2. The manual's bounds test. (a) The peak footprint under dlmalloc, counted by
   the test's own platform, within `HEAP_BYTES / 2`. (b) Keep
   `linked_list_allocator` as a conservative model and reword 8.1.

## The call

1. **Direct-map at 256 KiB and above** (the operator, 2026-09-24). ThylaAlloc
   gives a block of 256 KiB or more its own lazy reservation
   (`SYS_BURROW_RESERVE` at RW, with its alignment) and detaches it on free.
   GlobalAlloc passes the layout, so no header is needed; everything smaller is
   dlmalloc's.
2. **Peak use under dlmalloc** (the operator, 2026-09-24). 8.1 names dlmalloc,
   and `HEAP_BYTES` becomes the reader's working-set bound.
3. **The platform owns reservations** (scripture's own mapping, made sound; no
   vote). dlmalloc's `alloc` carves from the latest lazy reservation at a bump
   pointer. `free_part` decommits the tail and rolls the bump back, so the
   address space is reused, never leaked. `free` detaches a whole reservation.
   One reservation is one segment and one VMA: reserve, commit and decommit, as
   Windows, jemalloc's `retain` and Go's arenas do it.

## Rationale

The bar's second half is that relinquished memory returns and the footprint
shrinks. Without a large-block path, the programs that asked for the largest
fixed heaps (gallery 192 MiB, view 64 MiB, halcyond 64 MiB) are exactly the ones
whose freed buffers can stay pinned below a live block. The port's omission is a
wasm-ism: wasm has no munmap. Restoring C dlmalloc's own threshold one layer up
makes the allocator the production allocator ARCH names. Fragmentation among
small blocks remains dlmalloc's documented behaviour, as it is in glibc's main
arena. The reservation layer is what makes `free_part` = decommit sound, and it
also makes growth cost no syscall and no VMA.
