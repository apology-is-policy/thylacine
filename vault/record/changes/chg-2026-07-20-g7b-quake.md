---
id: chg-2026-07-20-g7b-quake
type: chg
title: "G-7b: the stdio openers through the patched open()"
date: 2026-07-20
arc: arc-tapestry
commits: ["908d8bc6"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
`fopen`, `__fopen_rb_ca`, and `tmpfile` call musl's `sys_open` MACRO --
a raw `__syscall(SYS_openat, ...)` that bypasses the boundary-line
`openat()` FUNCTION entirely and dies on the sentinel. No pouch program
had ever fopen'd by path (stratumd uses `open()`; the probes use stdio on
inherited fds), so the gap sat latent until TyrQuake's
`COM_FileOpenRead` silently found no pak file.

Only the READ arm works here: create-mode `fopen` and `tmpfile` now fail
with an honest `ENOTSUP` instead of a blanket `ENOSYS`, waiting on the
`SYS_WALK_CREATE` wiring that CL-1a brought.
