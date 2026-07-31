---
id: fnd-term4-self-sa1
type: fnd
title: "G2 consume snapshotted fid_gen AFTER the take — a window event escaped the donate gate"
round: adt-term4-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-14-term4-close
regression: ""
created: 2026-07-31
---
## Prosecution

The G2 consume path captured `fid_gen` AFTER `dirfid_take` (and after
two allocations), so an invalidation landing in the serve→take window
fell OUTSIDE the donate gate's later `(fid_gen, gen]` staleness scan —
the gate leaned on the create/mkdir drop-hook backstop for exactly that
window. SELF-FOUND (the concurrent self-audit); both prosecutors then
traced the backstop sound (the dead qid is dentry-unreachable, so the
un-gated window could never produce a wrong SERVE — a bounded fid-park
gap, not a stale read).

## Disposition

Fixed: the snapshot moved BEFORE the take — a window event now yields at
worst a FALSE-STALE clunk (the fail-safe direction; one wasted fid), and
the gate is airtight at its own layer instead of borrowing soundness
from a neighboring hook. The defense-in-depth rule: each layer's gate
should be sound in isolation even when a backstop exists.
