---
id: fnd-net2d-r1-f3
type: fnd
title: "A rejected connect burned an ephemeral port; a rolled-back clone over-counted `opened`"
round: adt-net2d-r1
severity: P3
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-17-net2-netd-birth
created: 2026-07-31
---
## Prosecution

`ctl_connect` advanced the ephemeral rotation BEFORE the connect
attempt, so a rejected re-connect burned a port from the 16k pool; and
the h_lopen clone rollback decremented `active` but not `opened`,
skewing the stat. (Absorbs the self-audit's SF1.)

## Disposition

Fixed in the close: peek-then-commit on `next_local_port` (the
rotation commits only on connect Ok) + `clone_rollback` uncounts
`opened` symmetric to the mint.
