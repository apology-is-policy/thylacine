---
id: chg-2026-07-13-g2-dirfid
type: chg
title: "G2: the dir-fid cache -- 0-RT repeat directory resolution"
date: 2026-07-13
arc: arc-go-build
commits: ["07a0adce", "65ef4675"]
touched: [sub-kernel-ninep-dev9p, sub-kernel-ninep-client]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
Walk-fresh unopened DIRECTORY fids park in a 64-entry per-client cache at
close and re-issue at the next bind-form resolve with zero wire ops. The
three-layer stale-fid defense (mutation drop hooks / the donate staleness
gate over the G4 ring / the fid_suspect backstop). The term-4 close
(`65ef4675`, 0/0/1/3: F1 [P2] = the gen snapshot moved BEFORE the take --
an invalidation in the serve->take window must fall inside the donate
gate's scan) closed in-commit; its per-finding backfill lands with the
larder sweep's closed-list absorption.
