---
id: chg-2026-06-03-stalk3c-retire
type: chg
title: "stalk-3c: retire the three name-only /srv syscalls; the namespace is the API"
date: 2026-06-03
arc: arc-identity-detour
commits: ["0d360f76", "8c834de5", "d26e7607", "cde35777"]
touched:
  - sub-kernel-devsrv
established: []
closed: [fnd-stalk3c-r1-f1, fnd-stalk3c-r1-f2, fnd-stalk3c-r1-f3]
opened: []
mirrors-checked: []
depth: skeletal
---
The ABI break that completed the stalk-3 arc: `SYS_POST_SERVICE` (26),
`SYS_SRV_CONNECT` (30), `SYS_POST_SERVICE_BYTE` (43) retired — numbers
reserved, no reuse, no compat shim — plus the dead client-KObj_Srv r/w
arms (−711 lines). 3c-a (`0d360f76` — corvus create=post pre-chroot),
3c-b (`8c834de5` — the pouch bind=create/connect=open seam + the
`sys_srv_peer` CSRVCLIENT gate), 3c-c-1 (`d26e7607` — kernel tests
migrated to the production cores), 3c-c-2 (`cde35777` — the retirement).
Audit [[adt-stalk3c-r1]] CLEAN 0/0/0/3 (all doc-staleness); I-1
prosecuted directly and STRENGTHENED — stalk-3 ARC COMPLETE.
