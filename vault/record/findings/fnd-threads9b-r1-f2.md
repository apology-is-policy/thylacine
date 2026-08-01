---
id: fnd-threads9b-r1-f2
type: fnd
round: adt-threads9b-r1
severity: P1
status: documented
title: "pthread stack guard pages are silently disabled"
surface: [sub-pouch-thread]
threatens: []
created: 2026-08-01
---
## Prosecution

1. musl's `pthread_create` maps `[map, map+size)` `PROT_NONE`, then
   mprotects `[map+guard, map+size)` to RW — the bottom stays
   inaccessible as the guard.
2. pouch's `__mmap` ignores `prot` and always returns RW (an anon Burrow
   is RW by construction; I-12 forbids X at attach).
3. pouch's `__mprotect` returns `-ENOSYS`, which `pthread_create`
   tolerates silently by design (`&& errno != ENOSYS`).
4. The whole region is RW including the guard bytes. A stack overflow
   corrupts the guard instead of faulting, and only faults after running
   past the ENTIRE region into an unmapped page.

The silence is the hazard: musl's tolerance clause exists for old
kernels, and it makes a missing guard indistinguishable from a present
one.

## Disposition

Documented, not fixed — the real fix needs a kernel syscall that can flip
VMA permissions PROT_NONE-capable, which is a v1.x lift
([[seam-pouch-guard-pages]]). Bounded at v1.0: the workloads do not
deep-recurse, and a sensitive one can buy headroom with
`pthread_attr_setstacksize`.
