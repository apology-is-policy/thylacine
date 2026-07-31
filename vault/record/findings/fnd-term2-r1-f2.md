---
id: fnd-term2-r1-f2
type: fnd
title: "The narrowing removes an incidental non-guaranteed heal of the Loom-bypass staleness"
round: adt-term2-r1
severity: P3
status: deferred
surface: [sub-kernel-larder]
threatens: [inv-i38]
seam: seam-larder-loom-bypass
created: 2026-07-31
---
## Prosecution

Under the old whole-parent drop, a SIBLING's synchronous mutation
incidentally force-re-walked a Loom-mutated name in the same directory —
an accidental, non-guaranteed partial heal of the Loom-bypass staleness.
The name-specific invalidation removes it: only the mutated name drops,
so a Loom-staled sibling binding now persists to LRU or its own next
sync mutation.

## Disposition

Deferred — folds into the tracked Loom-bypass seam (this is an
AGGRAVATION of that seam's window, not a new class; the heal was never a
guarantee anyone could rely on). Zero go-build effect (the build is pure
synchronous; no v1.0 consumer drives Loom FS mutations).
