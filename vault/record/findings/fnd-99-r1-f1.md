---
id: fnd-99-r1-f1
type: fnd
title: "EEXIST propagation alone leaves a stale negative dentry -- the race loser still fails"
round: adt-99-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-19-99-create-errno
regression: dev9p.create_errno_propagates_eexist
created: 2026-07-31
---
## Prosecution

Under a create race, the loser's earlier Open->ENOENT installed a
NEGATIVE (parent,name) dentry; its retry-Open after the -EEXIST serves
that stale negative RPC-free -> ENOENT -> the open-or-create spuriously
fails (go-fs 6b: 2/10 boots, boot-fatal). The success path already
dropped the dentry; the EEXIST arm returned NULL before reaching it.

## Disposition

Fixed: both dev9p_create failure arms, on -T_E_EXIST, call
larder_dentry_invalidate_name before returning -- EEXIST proves existence,
so any cached negative is stale; the invalidate also bumps the gen so a
concurrent negative-install snapshotted on the old gen is skipped.
Verified 10/10 under the 8-way race. Found convergently by the SMP gate,
the Fable holotype, and the self-audit -- same gap, same fix.
