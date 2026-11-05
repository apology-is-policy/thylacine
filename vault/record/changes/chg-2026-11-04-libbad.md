---
id: chg-2026-11-04-libbad
type: chg
title: "T_E_LIBBAD: an interpreter at fault says so"
date: 2026-11-04
arc: arc-phase6-pouch
commits: [d16cc271d]
touched:
  - sub-kernel-exec
established:
  - dec-2026-11-04-libbad
closed: [fnd-noexec-r1-f8]
opened: []
depth: skeletal
created: 2026-11-04
---
`exec_setup_from_spoor` tracks whose image it is refusing. `fnd-noexec-r1-f8` stood open as documented; this closes it. Regressions are legs of `execve.interpreter_says_why`, conditional on the phenotype bundle, with sabotages proving each ran.
