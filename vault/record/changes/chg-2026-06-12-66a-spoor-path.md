---
id: chg-2026-06-12-66a-spoor-path
type: chg
title: "#66a: the Spoor.path substrate + SYS_FD2PATH (I-33)"
date: 2026-06-12
arc: arc-holotype-rw
commits: ["a36b201e", "4f2328e1", "ffd224aa", "b50686c8"]
touched:
  - sub-kernel-path
  - sub-kernel-stalk
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The RW-12/W5-F3 container-keystone introspection half: `struct Path`
(refcounted, immutable, copy-on-walk — the Plan 9 `Chan.path`) carried
by every Spoor; the three resolver hook sites (stalk per-step + cross
transplant, walk-open, walk-create); the "/" attach seed;
`SYS_FD2PATH = 71`. Scripture-first (`a36b201e` + the attach-seed
refinement `4f2328e1`), then the impl + the close. Invariant
[[inv-i33]]: strictly non-load-bearing — a failed alloc leaves the name
NULL and the walk succeeds. The close (`b50686c8`) landed all 7
findings ([[adt-66a-r1]] 0/0/0/6 + the self-audit's totality guard
[[fnd-66a-r1-sa1]]): the one live wart was the open=connect adoption
arm installing the endpoint's born-"/" name ([[fnd-66a-r1-f2]] — the
transplant now carries the walked name; its owed regression
`stalk.path_adopt_transplant` was delivered at #66b). 8 `path.*` + 3
`stalk.path_*` tests; the #66b `/proc/<pid>/ns` consumer
(territory-side `mp_path`) pends the territory sweep's Record entry.
