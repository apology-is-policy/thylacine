---
id: chg-2026-05-19-srv-birth
type: chg
title: "P5-corvus-srv-impl (kernel half): the /srv registry + SrvConn transport + accept + peer identity"
date: 2026-05-19
arc: arc-corvus-srv
commits: ["8d675ed6", "0cd17e6e", "e5ad6c6d", "56b4a6f7", "4dc0f75b", "232c89b9"]
touched:
  - sub-kernel-devsrv
  - sub-kernel-srvconn
established: []
closed: [fnd-p5srv-r1-f1, fnd-p5srv-r1-f2, fnd-p5srv-r1-f6]
opened: [seam-srv-registry-lifecycle]
mirrors-checked: []
depth: skeletal
---
The kernel `/srv` stack's birth: a2 (`8d675ed6` — the devsrv Dev + the
service registry + the two-phase post + tombstoning), a3a (`0cd17e6e` —
the `SrvConn` object + the bidirectional byte transport + lifecycle),
a3b (`e5ad6c6d` — the accept backlog + the connect path + connection
Spoors + `SYS_SRV_ACCEPT`), a3c (`56b4a6f7` — `SYS_SRV_PEER` +
by-value server identity), b2 (`4dc0f75b` — `SYS_SRV_CONNECT` + the
KObj_Srv r/w arms + the per-Proc cap; all three later RETIRED at
[[chg-2026-06-03-stalk3c-retire]] / [[chg-2026-06-03-stalk3b-open-connect]]),
and the arc audit close (`232c89b9` — [[adt-p5srv-r1]]'s in-chunk
fixes). Prose lives in the commit messages; the corvus-userspace half of
the arc (a1, b1, b3a, b3b) backfills at the corvus sweep.
