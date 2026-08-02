---
id: seam-pouch-process-shared
type: seam
title: "`PTHREAD_PROCESS_SHARED` compiles, links, and does not synchronize"
status: open
surface: [sub-pouch-thread]
opened-by: chg-2026-05-23-p6-threads-b
tracker: "POUCH-DESIGN.md 8.2"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A cond / mutex / rwlock / barrier with the `pshared` attribute SETS the
attribute, but two Procs sharing a memory region will not synchronize
through it: torpor's wake set is keyed on the caller's Proc, so a wake
from A never reaches a waiter in B. The `priv` argument is discarded
throughout the retargeted layer, which is correct for the per-Proc
primitive and exactly why the cross-Proc case cannot work.

## The lift

A cross-Proc tier for torpor (keyed on a shared object rather than the
Proc), which is the same machinery a shared-memory IPC surface would
want. Not a v1.0 need: pouch has no `fork`, so the classic
map-then-fork-then-share pattern has no way to arise.
