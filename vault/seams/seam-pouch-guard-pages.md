---
id: seam-pouch-guard-pages
type: seam
title: "pthread stack guard pages are silently absent"
status: open
surface: [sub-pouch-thread, sub-pouch-process]
opened-by: chg-2026-05-23-p6-threads-b
tracker: "threads-9b F2"
created: 2026-08-01
updated: 2026-08-01
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
