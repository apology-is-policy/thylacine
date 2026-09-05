---
id: chg-2026-07-07-pounce
type: chg
title: "POUNCE P-2..P-5: fused walk+getattr resolution + SYS_STAT"
date: 2026-07-07
arc: arc-go-build
commits: ["a4a4d971", "d9fdff5e", "b2d21c68", "d7e543f9"]
touched:
  - sub-kernel-stalk
  - sub-kernel-ninep-client
  - sub-kernel-ninep-wire
  - sub-kernel-ninep-dev9p
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The metadata-RT collapse (the thylacine stalks per-component, then
POUNCES — one fused strike down the path). P-2 (`a4a4d971`): the
`Twalkgetattr 140`/`Rwalkgetattr 141` wire pair + `p9_client_walkgetattr`
(a standard #841 pipelined op; strict parser). P-3 (`d9fdff5e`): the
optional `Dev.walk_attrs` slot (dev9p + devramfs; the strict BIND/
partial/QUERY shape contract; the per-session `wga_unsupported` ENOSYS
latch — the netd lesson) + the stalk pounce block (run gather,
LEFT-TO-RIGHT fail-ordering post-scan, mount-mid-run split, carried
attrs, `logical_depth`) + `STALK_STAT`/`stalk_stat` + `SYS_STAT = 88`
(the 1-RPC, 0-fid path stat). P-4 (`b2d21c68` + the probe `d7e543f9`):
Go `os.Stat` → one SYS_STAT (was ~13 RPCs), libthyla-rs `fs::metadata`,
the pouch 0019 stat-family patch (pre-0019 the whole `stat(path)`
family was a silent ENOSYS hole). Measured: gofmt cold −19–21%, warm
Twalk 23.3k → 10, the Phase-2 attr cache DEFERRED on the recount (the
1:1 residual Tgetattr is the per-stalk base X-check, not the fd-fstat
class). P-1 is Stratum-side (`a14a2cf`, `h_walkgetattr`). Audit:
[[adt-pounce-p5]] CLEAN 0/0/0/1 — the P3 became
[[seam-372-latched-double-xcheck]].
