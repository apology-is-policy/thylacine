---
id: chg-2026-09-24-b1c-native-heap
type: chg
title: "B-1c (the native heap): thyla-heap -- dlmalloc 0.2.14 over reservations the platform owns, a block of 256 KiB or more its own mapping -- under libthyla-rs's ThylaAlloc; the fixed heaps, ThylaAllocN and slurp's cap gone; the manual's bound under dlmalloc; /heap-probe"
date: 2026-09-24
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-thyla-heap
  - sub-libthyla-rs
  - sub-manual
  - sub-gallery
  - sub-view
  - sub-halcyond
  - sub-kaua-term
  - sub-netd-nic
  - sub-haul
  - sub-stratum-boot
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
The native substrate's half of the ARCH 6.5 memory bar. libthyla-rs's fixed
4 MiB `linked_list_allocator` heap, and the fixed 16 to 192 MiB `ThylaAllocN`
heaps six programs declared, are replaced by thyla-heap (`usr/lib/thyla-heap`):
dlmalloc 0.2.14, vendored byte-identical from rust-src, over a platform that
owns its address space -- every segment carved at a bump pointer in the latest
lazy reservation (256 MiB, doubled per live reservation to 32 TiB, asked again
at half when refused), `free_part` decommitting the tail and rolling the bump
back, `free` detaching a whole reservation, and a carve never filling its
reservation so none can merge -- plus a mapping of its own for any block of
256 KiB or more (the operator's vote: C dlmalloc's threshold, which the Rust
port dropped). ARCH's literal mapping (a fresh attach per call) was shown from
dlmalloc's source to orphan a VMA per trim-and-release cycle; the reservation
layer is what makes `free_part` = decommit sound. The footprint (carved plus
direct bytes) and an exact peak are counted under the heap's lock, and the
manual's bounds test now bounds the reader's peak footprint on this heap
(MANUAL-DESIGN 8.1, the operator's second vote). `io::slurp` lost its 2 MiB cap
(a refused growth is still `NoMemory`). The witness is `/heap-probe` at boot:
64 MiB of small blocks raise the data view by 16432 pages and freeing them
leaves 14 with no trim asked for; a 32 MiB block returns every page while a
small one stays live; a reservation switch releases the emptied first one; the
churn either side of the 2 MiB trim threshold is timed (B-1b's F4). Host: nine
thyla-heap tests over a first-fit kernel model, eight platform sabotages red.
