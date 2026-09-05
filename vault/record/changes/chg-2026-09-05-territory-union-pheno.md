---
id: chg-2026-09-05-territory-union-pheno
type: chg
title: "sub-kernel-territory brought current: union mounts (the UM arc, seam closed), VIVARIUM Design D, the pheno-mount"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-kernel-territory
established: []
closed:
  - seam-union-mount-walk
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The first of the five VIVARIUM-D kernel giants (exec was done last session).
sub-kernel-territory was stale for ~1097 lines of churn since 2026-08-16 across
three features, all verified against the code before writing:

## Union mounts (the UM arc) -- and the stale seam this closes

The dossier said `MBEFORE`/`MAFTER`/`MCREATE` were "stored but never walked"
and pointed at [[seam-union-mount-walk]], whose own `status: open` was
FALSIFIED by landed code: territory.c (879-892) dispatches on the ordering
flags, `mount_members_snapshot` (territory.h 489) is the atomic whole-union
snapshot the resolver reads, stalk.c (289-320) crosses the first `MCREATE`
member, and `specs/territory.tla` grew a SEQUENCE-valued `mounts` with
`WalkFirstHit`/`ReaddirDedupFirstWins`/`CreateTargetCorrect`/
`RemoveTargetCorrect`/`OrderCorrect` + buggy cfgs. The UM arc built exactly
what the seam's "what closes it" prescribed (the ordering invariant, the
ordered lookup, the stalk cross, the MCREATE create path, the sequence model),
but no chg had closed the seam -- a stale seam falsified by landed code, which
the vault exists to catch. **Closed here.** The dossier now documents the
as-built union: the placement dispatch (MBEFORE prepend / MAFTER append,
searched in array order = OrderCorrect), MREPL replacing the WHOLE group
(UM-8 F6), the atomic snapshot (the UM-8 F4 hazard), MCREATE's writable-member
create (-T_E_ACCES when none). This file owns the ordering + snapshot;
[[sub-kernel-stalk]] owns the walk/readdir/create/remove (its own de-stale, owed).

## VIVARIUM Design D + the pheno-mount

- Design D: `Territory.flags` gains `TERRITORY_ROOT_PHENO_LINUX` (in the old
  alignment pad, offsets held); `territory_root_pheno`/`territory_declare_linux`;
  `stalk_core` seeds `crossed_pheno` from it at `restart:` (first pass + symlink
  re-anchor); the container declares on the namespace object because chroot swaps
  root_spoor; `/proc/<pid>/ns` renders `root: pheno-linux`.
- The pheno-mount: `MPHENO_LINUX`, per-mount-POINT scope detected by the
  RESOLVER (NOT the device-instance key, which would declare the whole session's
  pool); `/viv/bin` ships bare Linux binaries; fail-safe native; ungated
  (namespace edit confers no authority). Both OR-combined at the exec-time stamp;
  execve re-decides; the exec consumer is [[sub-kernel-exec]] (its Legs A/B +
  the RELEASE-store live in [[sub-kernel-proc]]).

## Invariants + frontmatter

Added the contract's fifth field (`flags`/root_pheno, live); I-3 note (a union
is a member SET at one identity, no cycle edge); I-28 (the pheno-mount half,
prose -- enforcement is stalk's); I-43 (the declaration channel, prose -- NOT
guarded-by, the exec precedent: territory declares shape, the fork enforces
non-escalation). `updated:` -> 2026-09-05.

## The other four giants remain

sub-kernel-{proc,stalk,vivarium,syscall-abi}: each a multi-feature de-stale,
one at a time, fresh context each (proc 1097 / stalk 1251 / vivarium 2671 /
syscall-abi 593 lines of churn). territory (475) was the tractable one this
session.
