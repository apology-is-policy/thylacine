---
id: lock-srv-admission
type: lock
title: "Service transport admission and retained domain bounds"
kind: spin
orders-before: []
guards: "Global and aggregate session connection reservations, each domain's charged count, and retained domain count."
created: 2026-10-01
updated: 2026-10-01
---
## Discipline

All applicable 16/48/64 counters change in one transaction before allocation.
The 16-domain bound also reserves before allocation. No memory allocation,
free, registry access or diagnostic-list insertion occurs under this lock.
A successful session ticket owns a domain ref. Unreserve updates counters
under the lock, then releases that ref outside it; final domain free precedes
returning its domain slot. The diagnostic-list lock may precede admission,
never the reverse. See [[sub-kernel-srvconn]].
