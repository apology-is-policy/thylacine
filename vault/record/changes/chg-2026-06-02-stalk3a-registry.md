---
id: chg-2026-06-02-stalk3a-registry
type: chg
title: "stalk-3a: the namespace-resident per-territory SrvRegistry + the boot /srv mount"
date: 2026-06-02
arc: arc-identity-detour
commits: ["23400e6e", "676567cc"]
touched:
  - sub-kernel-devsrv
established: []
closed: [fnd-stalk3a-r1-f1]
opened: []
mirrors-checked: []
depth: skeletal
---
The single static registry became a heap-allocated refcounted
`SrvRegistry` reached THROUGH the mounted devsrv root Spoor's `aux`
(Plan-9-true: named through the namespace, never a global); boot mounts
one immortal registry on kproc's `/srv`. Registry-ref discipline
(one ref per Spoor instance carrying `aux = reg`; the walk's
aux-normalize) + the per-instance `devno` ([[fnd-stalk3a-r1-f1]], fixed
in the close commit). Audit [[adt-stalk3a-r1]] CLEAN 0/0/0/4.
