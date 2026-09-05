---
id: chg-2026-07-11-d44-read-band
type: chg
title: "D44: aligned wire reads + attr-served EOF (+ the false-mid-file-EOF close)"
date: 2026-07-11
arc: arc-go-build
commits: ["cd4c1e9b", "eae0b613"]
touched: [sub-kernel-ninep-dev9p, sub-kernel-larder]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The CHASE D44 miss-class instrument found 82% of page-serve misses were
PERMANENT partial-page holes: the 131049-byte msize payload is not a
page multiple, sequential streams left every chunk's tail page partial,
and the aligned-start populate rule could never refill a partial FRONT
page — every re-read re-paid the wire for the whole tail (the go
toolchain's rodata/data segments, re-read multi-MB per exec). Two
guest-local dev9p fixes: (a) big unaligned reads on a cacheable client
wire at the containing page's ALIGNED start and return a legal short
read — holes heal in one pass; (b) `larder_attr_fresh_size` (the
cvers-GATED attr read) answers the sequential reader's final 0-probe
RPC-free for plain files. The audit close ([[adt-d44-r1]], Fable-5 max)
caught F1 [P1]: the `got <= lead → 0` arm manufactured a FALSE MID-FILE
EOF (a single Rread may legitimately short-return mid-file — the R-5
ground truth), whose consumers include the REVENANT cluster fill
(zero-filled RESIDENT text pages). Split true-EOF from short + retry
unshifted; plus F3: OTRUNC invalidated NOTHING (pre-existing since L1e —
truncate coherence had silently rested on an unverified cross-project
qid.version bump). Measured: warm S1 367 → 249 ms (the ≤266 bar
CROSSED); S3 cold 5486 → 4088.
