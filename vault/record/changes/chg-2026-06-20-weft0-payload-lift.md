---
id: chg-2026-06-20-weft0-payload-lift
type: chg
title: "Weft-0: lift the /net per-op payload 4 KiB -> 32 KiB + the TCP window -> 64 KiB"
date: 2026-06-20
arc: arc-weft
commits: ["d42e91be"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
Tier A of the throughput arc: grow the /net binding coherently across
the whole path — srvconn `SRVCONN_MSIZE` 4→32 KiB + ring cap 8→64 KiB
(kept 2× msize, pipeline depth unchanged), the kernel client's out_buf
8→32 KiB, and netd's `SRV_MSIZE` 8→32 KiB + TCP rx/tx 4→64 KiB +
`DATA_CHUNK` 4→32 KiB, with the two per-op recv scratches moved
stack→heap (a 32 KiB stack array would overflow netd's 256 KiB stack
and terminate the NIC owner). The 32 KiB ceiling (not the design
table's 64) is the inline srvconn ring bound of the era — later lifted
by CF-3 B's heap rings ([[chg-2026-07-08-cf3b-bulk-ring]]). The
kernel-side halves backfill with their sweeps' rows (srvconn's already
notes the era in its sizing history).
