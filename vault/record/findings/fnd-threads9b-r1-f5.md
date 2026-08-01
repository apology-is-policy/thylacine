---
id: fnd-threads9b-r1-f5
type: fnd
round: adt-threads9b-r1
severity: P2
status: fixed
title: "The build's seam-check list was not extended for the round's four new syscall numbers"
surface: [sub-pouch-seam, sub-pouch-thread]
threatens: []
fixed-by: chg-2026-05-23-p6-threads-b
regression: "the four new entries in the `build_sysroot` seam check"
created: 2026-08-01
---
## Prosecution

`build_sysroot` verifies the GENERATED `bits/syscall.h` against a static
list of expected `SYS_* number` pairs — the only defense against a musl
re-vendor silently losing an entry. The round added four numbers
(`torpor_wait 39`, `torpor_wake 40`, `thread_spawn 41`, `thread_exit 42`)
and did not add them to the list.

A typo swapping two of them would compile, link, and run — dispatching
the wrong kernel handler.

## Fix

The four entries added, and the `#define __NR_*` lines normalized to
single-space so the grep matches. The finding recurs verbatim one round
later ([[fnd-signals13b-r1-f1]]), which is what makes it a lineage rather
than an oversight: it is now a standing obligation on
[[sub-pouch-seam]].
