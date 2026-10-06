---
id: chg-2026-10-06-chdir-physical
type: chg
title: "chdir stores the name of where the walk landed"
date: 2026-10-06
arc: arc-identity-detour
commits: ["*(pending)*"]
touched:
  - sub-kernel-stalk
  - sub-kernel-territory
  - sub-kernel-syscall-dispatch
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
`SYS_CHDIR` validated a directory through the resolver, then stored a
lexically cleaned copy of the path. Once symlinks landed, the two named
different directories: `cd link/..` stored the link's lexical parent while the
resolver had climbed out of the link's target, and `getcwd` returned link
components. The operator chose the physical rule
([[dec-2026-10-06-chdir-physical]]; STALK-DESIGN 4.3).

The resolver now builds the name of where a walk lands, alongside its trail,
and change-directory stores it ([[sub-kernel-stalk]],
[[sub-kernel-syscall-dispatch]]). The lexical canonicalizer lost its one
caller and was deleted ([[sub-kernel-territory]]). ut's `cd` stays logical in
the shell, and `/bin/pwd` reports the kernel's name. The name is walked once
more before it is stored, so a node a union member shadows, which has no name,
cannot become the cwd.
