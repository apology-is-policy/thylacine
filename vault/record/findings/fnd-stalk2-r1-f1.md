---
id: fnd-stalk2-r1-f1
type: fnd
title: "The mount table admitted cycles — the I-3 'by construction' claim was false"
round: adt-stalk2-r1
severity: P2
status: fixed
surface: [sub-kernel-stalk]
threatens: []
fixed-by: chg-2026-06-02-stalk2
regression: "territory_mount.rejects_cycle (self-mount + a two-tree oscillation)"
created: 2026-08-01
---
## Prosecution

A self-mount (source identity == mount-point identity, reachable in ONE
`t_mount` call) or a cross-tree oscillation installed a cyclic mount.
`cross_mounts`' bounded loop then resolved it to a silently-WRONG
non-NULL endpoint — memory-safe (each iteration clunks the prior link)
but a namespace-integrity break and a false safety claim; a future
`PGRP_MAX_MOUNTS` bump would have turned it into a longer spin.

## Disposition

Fixed in the close commit: `territory.c::would_create_mount_cycle`
(mirroring bind's cycle check over the `(dc, devno, qid.path)`
mount-edge graph) rejects at `mount()` time — I-3 holds by ENFORCEMENT,
not construction; the loop bound stays as a defensive backstop.
Consequence honestly recorded: alloc-smoke's plumbing probe had
deliberately self-mounted and was retargeted. The fix's home is
territory.c (its dossier pends that sweep); the finding is filed on the
resolver whose crossing loop carried the false claim.
