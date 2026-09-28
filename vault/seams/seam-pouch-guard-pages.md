---
id: seam-pouch-guard-pages
type: seam
title: "pthread stack guard pages are silently absent"
status: closed
surface: [sub-pouch-thread, sub-pouch-process]
opened-by: chg-2026-05-23-p6-threads-b
closed-by: chg-2026-09-23-b1b-pouch-memory
tracker: "threads-9b F2"
created: 2026-08-01
updated: 2026-09-23
---
## Owed

musl's `pthread_create` allocates the stack `PROT_NONE` then mprotects
the usable part RW. pouch's `mmap` ignores `prot` (an anon Burrow is
always RW — I-12 forbids X at attach) and `mprotect` returns `ENOSYS`,
which `pthread_create` tolerates by design (`&& errno != ENOSYS`). Net
effect: the whole region is RW including the guard bytes, so a stack
overflow corrupts the guard instead of faulting — it only faults after
running past the ENTIRE region into an unmapped page.

Bounded at v1.0 (the workloads do not deep-recurse); the workaround is a
larger `pthread_attr_setstacksize`.

## The lift

A kernel syscall that flips VMA permissions (PROT_NONE-capable). The
same primitive retires the pouch `mprotect` sentinel and gives mallocng
its PROT_NONE metadata pages back.

## Closed (2026-09-23)

Closed by B-1b (patch 0044 `pouch-mman-protect-decommit`). The lift arrived at
B-1a as `SYS_BURROW_PROTECT` (ARCH 6.5 "The permission ceiling"); the seam's
own half is that pouch's `mmap` now mints at the prot asked for (`PROT_NONE`
reserves) and `mprotect` reaches the kernel, so musl's `pthread_create` — its
text untouched — gets the guard it always asked for: a `PROT_NONE` piece of the
stack mapping that a write faults in. `pouch-hello-guard` writes the first
usable byte of a worker's stack, then the byte below it, and dies of
`snare:segv`; `pouch-hello-threads` requires every worker's `---p` row of
exactly `guardsize` in `/proc/<pid>/maps`. mallocng's `PROT_NONE` metadata
pages came back with the same change.
