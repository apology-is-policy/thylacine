---
id: chg-2026-05-22-p6-syscall-seam
type: chg
title: "P6 sub-chunk 4: the syscall seam + the stdio backend"
date: 2026-05-22
arc: arc-phase6-pouch
commits: ["dbc6bd3e"]
touched:
  - sub-pouch-seam
established: []
closed: ["fnd-seam-r1-f1", "fnd-seam-r1-f6"]
opened: ["seam-pouch-errno-channel"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The boundary line itself: musl's aarch64 number table retargeted (eight
calls 1:1, everything else the `0xFFFF` sentinel), the sentinel guard on
both syscall paths, the flat-`-1` -> `EIO` decode, and stdio moved off
`writev`/`readv`. One mechanism carries P-1 and P-3 at once.

[[adt-seam-r1]] found the round's P0 in the half of the design that was
easy to miss -- the guards covered `__syscallN` but not the *cancellable*
path's hand-written asm ([[fnd-seam-r1-f1]]).
