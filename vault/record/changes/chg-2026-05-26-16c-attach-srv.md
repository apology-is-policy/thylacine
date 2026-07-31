---
id: chg-2026-05-26-16c-attach-srv
type: chg
title: "16c: the srvconn 9P transport + SYS_ATTACH_9P_SRV + SYS_PIVOT_ROOT + kernel_attached"
date: 2026-05-26
arc: arc-pouch-boot
commits: ["b1584c4a", "457f22d9", "97f569e2", "f05bdc5e", "fd706b36", "bd97a78c", "218feb0c"]
touched: [sub-kernel-ninep-transport, sub-kernel-ninep-attach]
established: []
closed: [fnd-16c-r1-f1, fnd-16c-r1-f2, fnd-16c-r1-f3, fnd-16c-r1-f4, fnd-16c-r1-f5, fnd-16c-r1-f8, fnd-16c-r1-f9, fnd-16c-r1-f10, fnd-16c-r2-f1, fnd-16c-r2-f2, fnd-16c-r2-f3, fnd-16c-r2-f4, fnd-16c-r2-f5]
opened: [seam-848-pivot-walk-race]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The second `p9_transport_ops` backend (byte-mode SrvConn rings) + the
syscall bridge from a client-held conn to a mountable dev9p root + the
pivot + the `kernel_attached` gate (a userspace close of the KOBJ_SRV
handle must not EOF load-bearing rings -- the chunk's smoking-gun bug).
Two audit rounds ([[adt-16c-r1]] DIRTY 0/3/4/6 -> [[adt-16c-r2]] CLEAN
0/1/1/4). Round-time fixes later REVERSED by design: the R1-F2/R2-F1R2
recv-deadline auto-arm was removed wholesale at #841 (a per-op timeout
desyncs the pipelined shared stream) -- the transport dossier records
today's truth; these notes freeze the round's. F12 (a joey-side rename)
backfills with the boot-chain sweep. F6/F11/F13 dispositions live in
their fnd notes.
