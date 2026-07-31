---
id: chg-2026-05-28-fs-alpha
type: chg
title: "FS-alpha: SYS_WALK_CREATE (create + mkdir-via-DMDIR)"
date: 2026-05-28
arc: arc-identity-detour
commits: ["3f039e67", "f3d6e5cf"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The first FS-mutation surface: the real `dev9p_create` (Tlcreate; Tmkdir
via the DMDIR fold with the walk-to-child fid swap) + the SYS_WALK_CREATE
handler. Scripture-first (`f3d6e5cf` the FS-mutation foundation, no code).
The A-1.5 audit close covering alpha+beta: `2081408d` (0 P0 / 0 P1 / 1 P2 /
3 P3; its per-finding record backfills if that surface is ever
re-prosecuted -- the closed list predates the vault's memory roster).
