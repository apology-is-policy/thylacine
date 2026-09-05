---
id: fnd-net2d-r1-f4
type: fnd
title: "Cross-call Treaddir coherency: a slot freed between paginated reads renumbers entries"
round: adt-net2d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

The readdir resume cookie is an ordinal over a live-slot scan; a slot
freed between two paginated reads shifts later ordinals. No UAF and no
stale RESOLUTION (single-threaded within a page; the walk filter
re-validates liveness) — a listing-consistency artifact only.

## Disposition

Closed justified: by-design, matching the kernel readdir-cookie
tolerance; a stable per-connection iteration snapshot is a v1.x item if
dir-read atomicity is ever needed. Carried as a
[[sub-netd-server]] caveat.
