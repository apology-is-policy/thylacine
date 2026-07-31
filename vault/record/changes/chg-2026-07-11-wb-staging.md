---
id: chg-2026-07-11-wb-staging
type: chg
title: "F1 write-behind: per-open-file append-run staging on loose mounts"
date: 2026-07-11
arc: arc-go-build
commits: ["f9c3cafe", "94409d56", "e0e8ebbb", "766410d4"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: [seam-wb-close-flush-slot]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The loose-writeback leg (Senate-voted; CHASE C-2: 97.9% of S3 Twrites were
<= 4 KiB). Scripture-first (`f9c3cafe` + the `e0e8ebbb` amendment: global
wb budget + the visible-run flush). Spec-first on the re-enabled L1b
surface: fs_cache.tla gained StageWrite/FlushClose + the skip_staged /
lost_stage buggy cfgs BEFORE the impl (`94409d56`). The SA-F1
single-flight race (a duplicate flush completing while the first's stale
residual chunks still fly) was self-found and closed in scripture
(@3c889c09). Measured: S3 med -445 ms at the C-2-F2 bar.
