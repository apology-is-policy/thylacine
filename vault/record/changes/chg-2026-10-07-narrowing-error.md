---
id: chg-2026-10-07-narrowing-error
type: chg
title: "The kernel builds with -Werror=shorten-64-to-32"
date: 2026-10-07
arc: arc-boosty
commits: *(pending)*
touched:
  - sub-kernel-notes
  - sub-kernel-mm-slub
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-07
---
The device number is `u64` and never reused ([[chg-2026-10-06-devno-u64]]), but
the build did not warn on a `u64` read into a `u32`, so a narrowed copy would
truncate it with no other symptom. Re-running devno-u64's census
(`-Wshorten-64-to-32`) found 12 narrowings, none a device number. Eleven read
the per-thread `note_mask` into `u32` locals; they were lossless, because an
assert in `notes.c` bounds every supported note bit by the `u8`
`Thread.note_claim`. The twelfth is `init_cache`'s slab object count,
`(PAGE_SIZE << slab_order) / actual`, which lands in the `u32` the slab page's
in-use `refcount` is compared against; at most 2^27 objects fit a slab of any
buddy order.

The 11 locals are `u64`. The slab count narrows explicitly, under a
`_Static_assert` pinned to `MAX_ORDER`; `slub.o`'s machine code is unchanged.
The kernel's compile flags carry `-Werror=shorten-64-to-32`: an implicit
narrowing fails the build, and an explicit cast still compiles.

The census takes its translation units from `kernel/CMakeLists.txt`
(`KERNEL_SRCS`, `KERNEL_TEST_SRCS` and the symbol-table stub), not from a
directory glob: a first pass globbed `kernel/` and `arch/arm64` only, never
compiled `mm/`, and reported 0 with the slab count still narrowing. It reads 0
over the 230 units of the test build, the 95 of the production build, with
`VIV_TRACE` and `THYLACINE_NO_TICKLESS`, and under each of the 8 fault-test
variants, which it checks against `tools/test-fault.sh`. A devno parameter
narrowed to `u32` fails the build ([[sub-kernel-notes]],
[[sub-kernel-mm-slub]]).
