---
id: seam-80-pivot-orphan-mounts
type: seam
title: "Pre-pivot mounts orphan but persist; the cap grows instead of a GC running"
status: open
surface: [sub-kernel-territory]
opened-by: chg-2026-05-26-16c-attach-srv
tracker: "task #80"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`territory_pivot_root` swaps the root and deliberately does not touch
`mounts[]`. Entries whose mount POINTS lived under the old root become
unreachable — their `(dc, devno, qid.path)` identity can no longer be
produced by any walk from the new root — but they stay in the table,
holding a `spoor_ref` on their source and a `path_ref` on their
`mp_path`.

joey pays this in full: it mounts `/srv`, `/proc`, `/ctl`, `/dev`,
`/env` in the kproc boot namespace, pivots to the disk root, and
re-grafts each — so the cost is pre+post per re-grafted directory. That
accumulation, not any real namespace depth, is what drove
`PGRP_MAX_MOUNTS` from 8 to 16 to 20.

## What closes it

A pivot-time sweep that drops entries whose mount-point identity is no
longer reachable from the new root, releasing both refs. It would roughly
halve joey's live count and let the cap come back down.

The care needed is in "no longer reachable": an entry is orphaned
relative to a ROOT, and the test must not drop an entry that a surviving
bind or a sibling mount still reaches. Getting it wrong unmounts a live
graft — strictly worse than the leak.

## Risk while open

Bounded and static: the waste is at most a fixed number of entries and
their refs, all released at Territory final release, and the only Proc
that pivots is init. It becomes real if pivoting ever generalizes to
ordinary Procs (a container runtime pivoting per instance), where the
per-pivot residue would be paid per container and the fixed cap becomes
a hard ceiling on how many mounts a pivoting Proc may hold.
