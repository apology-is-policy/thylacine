---
id: chg-2026-07-13-g1-write-populate
type: chg
title: "G1: own-page install at the wb flush + range-scoped write-through invalidate"
date: 2026-07-13
arc: arc-go-build
commits: ["5c4e5736"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The flush's per-flush page invalidate was DISCARDING the bytes the build
reads back (the ~100-byte buildid pwrite's whole-file invalidate nuked
each freshly-written archive: b009/_pkg_.a wire-read 976x/22.7MB in one S3
window). G1a: a FULL-land flush installs the run's pages as OWN (the
err==0 coupling pinned by fs_cache_buggy_populate_unflushed); G1b: the
write-through invalidate is range-scoped to [offset, offset+accepted).
Measured: S3 wire reads 6.7k -> 3.5k, read bytes 112 -> 31 MB.
