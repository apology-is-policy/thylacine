---
id: chg-2026-08-01-territory-sweep
type: chg
title: "The territory sweep: the namespace tables, the two locks, and a dead bind graph"
date: 2026-08-01
arc: arc-vault
commits: []
touched:
  - moc-kernel-namespace
  - inv-i1
  - inv-i33
  - arc-holotype-rw
established:
  - sub-kernel-territory
  - spec-territory
  - inv-i3
  - lock-territory-ns-lock
  - lock-territory-dot-lock
  - arc-phase5-namespace
  - arc-life-support
  - arc-clade
  - seam-union-mount-walk
  - seam-rfnameg-shared-territory
  - seam-80-pivot-orphan-mounts
  - seam-handle-based-dot
  - seam-mount-graph-unmodeled
  - seam-66c-proc-fd
  - view-closed-sub-kernel-territory
closed: []
opened: [seam-union-mount-walk, seam-rfnameg-shared-territory, seam-80-pivot-orphan-mounts, seam-handle-based-dot, seam-mount-graph-unmodeled, seam-66c-proc-fd]
mirrors-checked: []
depth: rich
---
## What

Sweep batch 6 — the STATE half of the namespace area
(`kernel/territory.c` 988 + `territory.h` 482 + the six territory-facing
syscall handlers, read in full per the standing sweep bar; plus
`specs/territory.tla` 504 and its six cfgs).

Present: [[sub-kernel-territory]] (the mount table's Plan 9
`(dc, devno, qid.path)` keying and why all three components are
load-bearing; MREPL displacement; BOTH cycle checks; chroot vs pivot as
one transition under two preconditions; `territory_clone`'s three ref
classes plus the cwd snapshot and its OOM rollback; the final-release
ordering; the lexical cwd resolver; `territory_format_ns`), plus
[[spec-territory]], [[inv-i3]], and the two lock notes
[[lock-territory-ns-lock]] / [[lock-territory-dot-lock]] — the first
lock notes for a per-Proc-group structure rather than a device.

Two standing hooks DISCHARGED: [[inv-i1]] had a backfill note saying its
primary enforcement surface was the Territory layer and would join at
this sweep; [[inv-i33]] owed the `PgrpMount.mp_path` edge. Both now
carry their territory half, and [[inv-i1]] moves from `strength: prose`
to `spec`.

Record: three arcs opened ([[arc-phase5-namespace]] — the mount table,
the mount syscalls, the root pivot; [[arc-life-support]]; [[arc-clade]])
plus 6 retro chgs, 3 adts, and 10 fnds. Six seams minted, of which
[[seam-mount-graph-unmodeled]] is the one that matters.

`docs/reference/18-territory.md` + `56-sys-mount.md` STUBBED (absorbed).

## Why

The recorded batch-6 target, chosen over pouch because it re-homes the
one seam the stalk sweep corrected ([[seam-848-pivot-walk-race]], closed
by RW-4's `ns_lock`) and discharges the two invariant hooks that sweep
left dangling.

Reading the code found something the docs cannot: **the bind table is
structurally dead.** There is no `SYS_BIND`; `bind()`/`unbind()` have no
caller outside `territory.c` and the tests; and neither `stalk.c` nor
`syscall.c` so much as names `binds`, `PgrpBind`, or `path_id_t`. What
the boot chain calls "binding `/bin`" is `mount(..., MREPL)`. The table
is allocated, cloned, cycle-checked, size-asserted, and rendered as a
count — while being unreachable and unread.

That is not tidiness. `specs/territory.tla::NoCycle` — the model's ONLY
cycle invariant — ranges over `bindings`. So the one cycle property the
spec proves is about the dead table, while the LIVE mount-graph check
(`would_create_mount_cycle`) is unmodeled — and that check exists
precisely because [[fnd-stalk2-r1-f1]] falsified the claim that
[[inv-i3]] held "by construction" on the mount table. The exact class of
assumption a model exists to break is the one the model does not cover.

The staleness verdict was the sharpest yet. `18-territory.md` was wrong
about both struct sizes, the mount cap, the whole mount-cycle check, and
five API surfaces — and was internally self-contradictory (its struct
listings omitted `mp_path` and `ns_lock` while its own Status table said
both had landed). `56-sys-mount.md` was stale in a DIFFERENT mode worth
naming: PARTIALLY updated, with a stalk-2-current ABI block sitting on
top of a P5-era tail that still taught that walking a mount point "uses
the Plan 9 bind table". A current section lends authority to the stale
ones beneath it.

## Verification

`quaestor lint --all` green through the fail-closed hook; views
re-rendered (dashboard, seams, invariants, locks, audit-triggers,
roadmap + the new closed-preamble view). Sabotage revert-probes each
failed as designed and were restored clean.

Every claim in the dossier traced to current source rather than to the
closed lists, which are 49-52 days old and wrong in the expected
direction: the LS-4 list correctly recorded the lockless `root_spoor`
read as the pre-existing #848 race, which RW-4 closed the following day;
the #66b list quotes `PGRP_MAX_MOUNTS=12` and a 600-byte Territory
(now 20 and 920). Retro SHAs verified against `git log` (and one
corrected — the stub-e2 hash fixup is `1ae21e5c`, not the `1ae21e5b` a
transcription produced). The test roster (5 `territory.*` + 13
`territory_mount.*` + 11 chroot/pivot = 29) counted from
`kernel/test/test.c`. The dead-bind-table claim verified three ways:
no `SYS_BIND` in the syscall header, no production caller, no reference
from the resolver.
