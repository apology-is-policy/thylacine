---
id: chg-2026-09-25-b1d-round2-close
type: chg
title: "B-1d holotype round 2 close: the bin/ move's four breakers, the escape leg walks its .., the initrd root guarded (4 P0-class closed in the round + 0 P0 / 0 P1 / 1 P2 / 5 P3)"
date: 2026-09-25
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-joey
  - sub-pouch-mem
  - sub-stratum-boot
  - sub-kernel-territory
  - sub-substrate-remote-host
  - sub-coreutils-filters
established: []
closed: [fnd-b1d-r2-f1, fnd-b1d-r2-d5]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Closes [[adt-b1d-r2]]. The four breakers the bin/ move made were fixed in WIP 8
(0119d242), among them [[fnd-b1d-r2-d5]], a name handed to a primitive that
walks one component. The confined leg walks its `..` again
([[fnd-b1d-r2-f1]]); the build refuses anything at the initrd root beside
`bin/` and `lib/`; joey checks that its working directory is `/` after the
pivot; and the stale prose was corrected. Two pre-existing P3s went to
OPEN-BUGS.
