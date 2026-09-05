---
id: chg-2026-06-09-957-single-hop-cross
type: chg
title: "#957: the single-hop SYS_WALK_OPEN crosses mounts"
date: 2026-06-09
arc: arc-identity-detour
commits: ["8301e8d7"]
touched:
  - sub-kernel-stalk
  - sub-kernel-devsrv
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Plan 9 has no non-crossing walk — but the single-hop `SYS_WALK_OPEN`
(the primitive libthyla-rs `fs::` navigates with) did not consult the
mount table, so a logged-in user's `mkdir`/`touch` into `/home/<user>`
(a per-user dev9p mount over a SYSTEM-owned placeholder) resolved the
SHADOWED placeholder and was denied by A-3 rwx. Fix: `cross_mounts`
made public (`stalk_cross_mounts`) and the handler crosses at BOTH the
SOURCE (before X-search + walk — stalk's base-cross mirror) and the
RESULT (before open — the quarry-cross mirror). The in-cycle audit
([[adt-957-r1]], 0/1/0/2) caught the pre-existing open-return-bool bug
the new reachability exposed: a single-hop open of a `/srv/<name>` leaf
leaked the devsrv connection endpoint ([[fnd-957-r1-f1]]); devsrv also
gained the registry-root-as-dir open + `stat_native` (the `cd /srv`
prerequisite of the still-open [[seam-932-devsrv-readdir]]).
