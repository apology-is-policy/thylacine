---
id: chg-2026-09-23-b1b-pouch-memory
type: chg
title: "B-1b (the Pouch memory seam): mmap at the asked prot, mprotect / madvise / MAP_FIXED / partial munmap over RESERVE / PROTECT / DECOMMIT / DETACH, mallocng's MADV_FREE on, the real pthread guard, the 8 MiB main stack with its extent in auxv, __init_tls through __mmap, the phenotype madvise row"
date: 2026-09-23
arc: arc-boosty
commits: ["037f511d"]
touched:
  - sub-pouch-mem
  - sub-pouch-thread
  - sub-pouch-process
  - sub-pouch-seam
  - sub-kernel-exec
  - sub-kernel-burrow
  - sub-kernel-vma
  - sub-kernel-vivarium
  - sub-stratum-boot
established: []
closed: [seam-pouch-guard-pages]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-23
---
The Pouch substrate's half of the ARCH 6.5 memory bar. Three new patches on
the musl series: 0044 makes the mman lower half exact -- `mmap` mints at the
prot asked for over `SYS_BURROW_RESERVE` (PROT_NONE reserves, W implies R, X is
EACCES), `mprotect` is `SYS_BURROW_PROTECT` with four arguments under Linux's
normalisation, `madvise` DONTNEED / FREE is `SYS_BURROW_DECOMMIT`, `MAP_FIXED`
over one's own anonymous mapping is discard + reprotect and never a creation,
partial `munmap` is the range detach, and mallocng's `USE_MADV_FREE` is on so a
freed slot inside a retained group returns its whole pages; 0045 has
`pthread_getattr_np` derive the main stack from the kernel's new auxv pair
`AT_STACK_BASE` / `AT_STACK_SIZE` (the stack is 8 MiB since this change) and
refuse a kernel that writes neither; 0046 routes `__init_tls`'s raw
six-argument `SYS_mmap2` through `__mmap`, REQUIRED by 0044 parking
`__NR_mmap` back at the sentinel (the call had reached 83, whose kernel arm
has read that Linux shape since Clade CL-4; parked, it would get ENOSYS and
die before `main`). Kernel side:
`burrow_decommit_in` returns Linux's errnos behind a `sys_burrow_decommit_core`
(the native 84 still answers 0 / -1), `vma_range_is_mapped_in`, and the
phenotype `madvise` row (233) pulled forward so a Linux guest's allocator
returns pages like the other two substrates; the row declines a mapped range
outside the burrow window and answers ENOMEM for a hole wherever it lies. The
provers: `pouch-hello-mem` (twelve legs; the mallocng-trim witness measured
4263 -> 2339 -> 11 pages full / half-freed / all-freed, identical at both smp
counts), `pouch-hello-guard` (a write into a worker's guard dies of
`snare:segv`), three legs on `pouch-hello-threads` (8 MiB, a 4 MiB frame, every
worker's `---p` guard row), pheno-probe legs L23j-L23t. Seven sabotages, each
scored by the lines that went red in its own boot log: `USE_MADV_FREE 0`, the
madvise release at the sentinel, `mmap` minting RW whatever the prot,
`PROT_NONE` minted RW (the guard row `rw-p`), the auxv pair mis-tagged, 0046
dropped from the series, the phenotype arm answering 0 without the core. The
sysroot's SEAM check pinned `SYS_mmap 83` by literal and is re-pointed at the
sentinel plus the internal header's four numbers. The seam
`seam-pouch-guard-pages` closes in place.

Holotype round 1 (Fable 5.1; closed on Opus 5.5): 0 P0 / 0 P1 / 1 P2 / 5 P3.
F1 withdrew this change's own headline: `__init_tls`'s raw call had been
served by the kernel's CL-4 arm all along, so 0046 is required by the
parking, not by a crash -- the text above says so. F2 made the guard child's
positive control observable (an eighth sabotage, `guardlow`, reddens it by
name); F3 (MAP_FIXED's post-discard refusals), F4 (the eager release's
churn) and F6 (three phenotype divergences) are documented; F5 fixed stale
comments.
