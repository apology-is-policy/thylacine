---
id: chg-2026-05-25-16b-gamma-mount-close
type: chg
title: "16b-gamma-mount-close: abort() stops extincting the kernel"
date: 2026-05-25
arc: arc-pouch-boot
commits: ["51ce0b96"]
touched:
  - sub-pouch-process
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
musl's `abort()` reaches `a_crash()` -- a deliberate NULL deref -- and
under v1.0's `FAULT_UNHANDLED_USER` policy an EL0 fault takes down the
whole kernel, so every `assert()` in every pouch program was a boot kill.
0011 routes abort straight to `_Exit(127)`, the status musl itself
reaches at the bottom of the same function.

Landed here because stratumd's mount path has asserts, and the joey probe
could not flip from NON-FATAL to FATAL until they failed cleanly. Carried
an explicit safe-use envelope (single-thread contexts only) until #809's
`SYS_EXIT_GROUP` made `_Exit` safe with live peers.
