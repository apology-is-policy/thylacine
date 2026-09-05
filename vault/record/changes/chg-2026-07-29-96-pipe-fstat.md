---
id: chg-2026-07-29-96-pipe-fstat
type: chg
title: "#96: fstat on a pipe — the third door of the CL-4 masking layer"
date: 2026-07-29
arc: arc-clade
commits: ["30252a52"]
touched: [sub-kernel-pipe]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

devpipe had no `.stat_native`, so `SYS_FSTAT` on a pipe returned -1.
Latent for the tree's whole life because no pouch program ever had a
pipe on fd 0/1/2 at startup — until the CL-5 build storm: GNU
`make -jN` hands every concurrent job but one the read end of a
broken pipe as stdin (`get_bad_stdin`), and clang treats a non-EBADF
fstat failure on a standard fd as FATAL. Job 1 built; every sibling
died silently. The same door had been fixed twice before (console,
/dev) — the pipe was the third.

The fix: `T_S_IFIFO | 0600`, size 0 (a buffered-count report would
invite a read sized against it — racing the peer by construction),
blksize 4096, and a monotonic `qid.path` stamped into BOTH ends (one
pipe, one inode; starts at 1 so the historical unset 0 stays
distinguishable). Every existing `qid.path` consumer was swept for
collision before stamping — and the stamp is monotonically SAFER
than before, when every pipe carried 0 and any keying that saw a
pipe was already colliding.

`.seekable` stays false: fstat-able and seekable were deliberately
decoupled at RW-4 R2-F2, and `sys_prw.pipe_not_seekable` pins it.
