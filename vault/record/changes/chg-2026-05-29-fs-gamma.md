---
id: chg-2026-05-29-fs-gamma
type: chg
title: "FS-gamma: SYS_RENAME + SYS_UNLINK"
date: 2026-05-29
arc: arc-identity-detour
commits: ["163b16bc", "92522f0e", "b780b722"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
`dev9p_rename` (Trenameat, same-session gate) + `dev9p_unlink` (Tunlinkat,
the REMOVEDIR flag equality pinned) -- borrow-only fid discipline (no
transient fid, the create-leak class structurally absent). Scripture
`163b16bc`, impl `92522f0e`, audit close `b780b722` (opus R1 CLEAN
0/0/0/3). Pulled forward from the coreutils roadmap so A-1b persistence
could rename-swap (the chunk-completeness rule's worked example).
