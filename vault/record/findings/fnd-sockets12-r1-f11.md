---
id: fnd-sockets12-r1-f11
type: fnd
round: adt-sockets12-r1
severity: P3
status: fixed
title: "bind/connect accepted an unterminated sun_path, passing caller stack as a service name"
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-05-23-p6-sockets
created: 2026-08-01
---
## Prosecution

`sun_path_to_name` scanned for a NUL within the bounded length and
accepted `n == avail` — no terminator found. A `sun_path` filled with
non-NUL bytes to the end of the structure was therefore accepted, and the
bytes past the intended name are whatever the caller's stack held.

The kernel validates each byte, so this is a POSIX-divergence and an
information-shape bug rather than a memory-safety one.

## Fix

Require explicit NUL termination within `avail`; `n == avail` returns
NULL and the caller surfaces `EINVAL`. The same discipline is carried
into net-5's `sun_path_split`.
