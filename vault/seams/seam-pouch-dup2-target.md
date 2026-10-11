---
id: seam-pouch-dup2-target
type: seam
title: "Pouch target duplication qualification and remaining socket scope"
status: open
surface: [sub-pouch-process]
opened-by: chg-2026-07-23-cl1b-process
tracker: "CL-1b"
created: 2026-08-01
updated: 2026-10-15
---
## Owed

Native target duplication is implemented through approved native 131 (codex/astra 130) and Pouch 0055;
bounded host/kernel/guest qualification passed. Tagged socket duplication is still explicitly
unsupported. Public descriptor flags are implemented, but raw internal fcntl
users remain gaps; do not call full freopen/login_tty/daemon/wordexp qualified
because one primitive now exists.

## The lift

`handle_dup_to_checked` exposes the existing atomic replacement core through
native 131 (HANDLE_DUP_TO; codex/astra 130), with flags from the caller and unchanged alias/rights rules. Self-dup
remains the separately validated 0054 behavior. See
`docs/NATIVE-DESCRIPTOR-OPERATIONS-REVIEW.md` and its source-matched evidence.
