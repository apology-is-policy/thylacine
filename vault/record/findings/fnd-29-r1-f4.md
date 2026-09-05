---
id: fnd-29-r1-f4
type: fnd
title: "Stale test-file comment described the retired 256-entry inline shape"
round: adt-29-r1
severity: P3
status: fixed
surface: [sub-kernel-larder]
threatens: []
fixed-by: chg-2026-07-11-fid-lifecycle
regression: ""
created: 2026-07-31
---
## Prosecution

The test file's header still described the L1c-era "~42 KiB inline
256-entry" shape after two re-sizes — a reader calibrating expectations
(capacity, eviction behavior) against it would reason about a structure
that no longer exists.

## Disposition

Fixed in-round: the comment updated to the heap-lazy 4096/4096/32768
shape. Doc-drift class; no behavior.
