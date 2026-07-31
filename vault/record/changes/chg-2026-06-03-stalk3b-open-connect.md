---
id: chg-2026-06-03-stalk3b-open-connect
type: chg
title: "stalk-3b-beta: open=connect + the 9P-unification + the embedded-client retirement"
date: 2026-06-03
arc: arc-identity-detour
commits: ["995973dd", "cd40f64c", "42ce2e0b", "80035641", "46ff3780"]
touched:
  - sub-kernel-devsrv
  - sub-kernel-srvconn
  - sub-kernel-ninep-attach
established: []
closed: [fnd-stalk3b-r1-f1, fnd-stalk3b-r1-f2, fnd-stalk3a-r1-f4]
opened: [seam-srv-9p-connect-unit]
mirrors-checked: []
depth: skeletal
---
`open("/srv/<name>")` IS the connect: A (`995973dd` — STALK_OPEN adopts
a Dev.open-returned replacement Spoor), B (`cd40f64c` — the
`devsrv_open_connect` core: 9P-mode → a dev9p root via
`srvconn_attach_dev9p_root`, byte-mode → a CSRVCLIENT conn Spoor), C1
(`42ce2e0b` — `SYS_ATTACH_9P_SRV` retargeted KObj_Srv → KOBJ_SPOOR;
joey migrated), C2 (`80035641` — corvus connects migrated), D
(`46ff3780` — the embedded per-SrvConn 9P client RETIRED, the SrvConn
becomes pure transport + identity; the per-Proc conn cap removed,
closing [[fnd-stalk3a-r1-f4]]). The F1 `kernel_attached` I/O guard on
the CSRVCLIENT branches landed in the close ([[adt-stalk3b-r1]] CLEAN
0/0/1/2).
