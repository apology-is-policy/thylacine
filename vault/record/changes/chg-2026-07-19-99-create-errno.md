---
id: chg-2026-07-19-99-create-errno
type: chg
title: "#99: propagate the real create errno + drop the stale negative dentry on EEXIST"
date: 2026-07-19
arc: arc-go-build
commits: ["de3b97fe"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: [fnd-99-r1-f1, fnd-99-r1-f2, fnd-99-r1-f3, fnd-99-r1-f4]
mirrors-checked: []
opened: []
depth: skeletal
created: 2026-07-31
---
`dev9p_create` recorded every failure as NULL and the handler returned the
bare -1 (== generic EPERM sentinel), so a racing create's -EEXIST reached
EL0 as EPERM and gopls logged "operation not permitted". The fix threads
the real errno through the transient `create_errno` + clamps to the
[-4095,-2] passthrough window. The round's P1 ([[fnd-99-r1-f1]]) was the
deeper half: errno alone was insufficient under the race -- the loser's
own Open->ENOENT had installed a NEGATIVE dentry its retry then served
RPC-free. Fable holotype + self-audit + the SMP gate converged on the same
gap and fix ([[adt-99-r1]]).
