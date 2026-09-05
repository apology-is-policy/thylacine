---
id: chg-2026-05-26-16bg-hardening-3b
type: chg
title: "P6 hardening #3b: mallocng's assert stops extincting the kernel"
date: 2026-05-26
arc: arc-pouch-boot
commits: ["c4d2cae7"]
touched:
  - sub-pouch-process
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The same `a_crash()` -> `_Exit(127)` swap for mallocng's own internal
`assert` macro, which 0011 did not touch (mallocng's macro is distinct
from libc's). Split into its own patch deliberately: it changes a macro
every allocation instantiates, so a future audit can reason about the
hot-path change alone.

Landed while the AEGIS/mallocng corruption was still open -- so that the
next recurrence would produce a clean `rc=127` joey could reap, instead
of a kernel extinction with no information.
