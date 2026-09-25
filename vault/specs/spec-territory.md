---
id: spec-territory
type: spec
title: "territory.tla"
models: [sub-kernel-territory]
pins: [inv-i1, inv-i3]
cfgs:
  - "territory.cfg -- clean, SYMMETRY Symm: every invariant below, at 2 Procs x 2 Paths x 2 Spoors x 2 covered dirs x 1 Name"
  - "territory_file_point.cfg -- clean, FilePaths = {b}: ordered mounts at a file point stay plain"
  - "territory_cov_alias.cfg -- clean, COV_MOUNTABLE: a covered directory also mounted at another point"
  - "territory_buggy.cfg -- BUGGY_CYCLE: bind without the cycle check; two binds compose into a loop (NoCycle)"
  - "territory_buggy_mount_no_refbump.cfg -- mount adds the entry, skips the ref bump (MountRefcountConsistency)"
  - "territory_buggy_unmount_no_refdrop.cfg -- unmount removes the entry, skips the ref drop (leak)"
  - "territory_buggy_destroy_leak.cfg -- final release clears the mount table without dropping refs"
  - "territory_buggy_chroot_no_refbump.cfg -- chroot stamps root_spoor without the bump or the drop-of-old"
  - "territory_buggy_mount_order.cfg -- an MBEFORE member appended (OrderCorrect)"
  - "territory_buggy_walk_last_hit.cfg -- the union walk returns the last holder (WalkFirstHit)"
  - "territory_buggy_readdir_last_wins.cfg -- the union listing dedups to the last holder (ReaddirDedupFirstWins)"
  - "territory_buggy_create_any_member.cfg -- create ignores MCREATE (CreateTargetCorrect)"
  - "territory_buggy_remove_mcreate.cfg -- remove routed through the create member (RemoveTargetCorrect)"
  - "territory_buggy_union_no_covered.cfg -- the pre-vote union, no covered member (UnionHasCovered)"
  - "territory_buggy_fresh_after_remove.cfg -- a reposition judges freshness after removing the member (CoveredOnlyInUnion)"
  - "territory_buggy_covered_takes_flags.cfg -- the covered member carries the mount's MBEFORE / MCREATE (CoveredIsItsPoint)"
  - "territory_buggy_covered_last.cfg -- a fresh MAFTER puts the new tree ahead of the covered one (CoveredPlacement)"
  - "territory_buggy_unmount_orphans_covered.cfg -- unmount leaves the covered member alone (NoOrphanCovered)"
  - "territory_buggy_self_mount.cfg -- a point's own directory mounted at it as an ordinary member (NoSelfMount)"
  - "territory_buggy_cover_file.cfg -- a file point grows a covered member (NoCoveredFile)"
gate: "specs/check-territory.sh -- runs all twenty, pins the three clean distinct-state counts, and asserts WHICH invariant each buggy cfg violates (one worker, so the attribution is deterministic). Re-run it for ANY change to a mount-table / root_spoor mutation site (mount, unmount, the reposition arm, chroot, pivot_root, clone, final release) or to the union walk, listing, create or remove selection."
created: 2026-08-01
updated: 2026-09-25
---
## Abstraction

Namespaces are per-Proc function values over abstract paths; Spoors are
opaque names with a modeled refcount. Deliberately beneath the model: what a
mount POINT is (the model keys on an abstract `path`; the impl keys on the
Plan 9 `(dc, devno, qid.path)` triple since stalk-2), the `mp_path` names,
the cwd, and both locks.

Three layers:

- **Bookkeeping.** `refcount` is a SEPARATE variable from the mount table,
  and that separation is the point: keeping the counter independent of the
  cardinality it should equal makes "forgot to bump" and "forgot to drop"
  catchable as a desync rather than true by construction.
- **Unions (UM, 2026-09-02).** `morder[p][pt]` is the ORDERED member sequence
  at a point (`mb` MBEFORE, `mc` MCREATE), and `holds[s]` the names a member
  contains, fixed at Init and explored over every assignment. Walk, readdir,
  create and remove pick members by rule; the selectors are rule pins
  (definitional invariants with a buggy cfg each), not mechanism models.
- **The covered directory (B-1d-u, 2026-09-25).** An MBEFORE / MAFTER mount at
  a directory point hosting no member adds the point's own directory, a `cv`
  member drawn from `CovDirs` (disjoint from `Spoors`), in Plan 9's order.
  `FilePaths` are points that are not directories, where no union starts.
  `unioned` is a history variable: it separates a union that lost its covered
  member from an MREPL group that never had one, which look alike in `morder`
  alone. `COV_MOUNTABLE` lets a covered directory also be mounted elsewhere.

`SYMMETRY Symm` (Procs, Spoors) reduces the two large clean cfgs; the comment
at `Symm` argues its soundness (the one CHOOSE over model values ranges over
`[Paths -> CovDirs]`, which neither permutation touches). The buggy cfgs stay
unreduced, so their traces read directly.

## Action-site map

| Spec action | Impl |
|---|---|
| `Init` | `territory_init` / `territory_alloc` (via `territory_init_fields`) |
| `Bind` / `Unbind` | `bind` / `unbind` — the DEAD table (see below) |
| `MountBefore` / `MountAfter` | `mount`'s MBEFORE insert-at-group-front / append arms; `starts_union` plus the two `mount_install_at` calls place the covered entry, MCOVERED alone |
| `MountRepl` | `mount`'s MREPL arm (replace the whole group, covered entry included) |
| `Reposition` | `mount`'s #219 / F6 reposition arm: an existing member moves, and no covered entry is added |
| `Unmount` | `unmount`'s shift-down + the deferred `spoor_clunk`; the covered entry is never removed by name and leaves with the last mounted member |
| `Chroot` | `territory_chroot` AND `territory_pivot_root` — the same state transition under two preconditions |
| `ForkClone` | `territory_clone` (mount refs + root ref; the `mp_path` and `dot_path` copies are beneath the model) |
| `WalkSel` / `ReaddirSel` / `CreateSel` / `RemoveSel` | `kernel/stalk.c`'s union walk, the union readdir merge, `stalk_union_create_member` and `stalk_union_member_holding` |
| `BuggyDestroyLeak` | the counterexample to `territory_unref`'s final-release loop |

The covered member itself is crossed by `stalk_cross_src`, which clones it
without crossing into its own mount; that walk mechanics is beneath the model,
and the `stalk.union_covered_*` tests carry it.

## Known gaps

**`NoCycle` models the dead table.** The spec's general cycle invariant ranges
over `bindings`, and at v1.0 nothing populates `binds[]` — no `SYS_BIND`
exists and `bind()` has no production caller (see [[sub-kernel-territory]]
Caveats). The LIVE cycle risk is on the mount identity graph, guarded by
`would_create_mount_cycle`, added because [[fnd-stalk2-r1-f1]] showed I-3 did
NOT hold there "by construction". Since B-1d-u, `NoSelfMount` models that
check's shortest case, a point's own directory mounted at itself, which the
covered member made reachable; longer cycles still have no model. Tracked as
[[seam-mount-graph-unmodeled]]; the impl-side protection is the
`territory_mount.rejects_cycle` and `covered_self_mount_refused` tests and the
dossier's Prosecution list.

**Isolation is structural, not a state invariant.** [[inv-i1]] is encoded by
the data model — every action touches one Proc's slot — so a buggy variant
that updated two Procs in one step would need a temporal property to catch.
When RFNAMEG lands ([[seam-rfnameg-shared-territory]]) the sharing becomes
real and Isolation must become a checked invariant.

**Neither lock is modeled.** `ns_lock` and `dot_lock` serialize what the model
treats as atomic actions. The RW-4 SA-F1 race ([[fnd-rw4-sa-f1]]) — a peer
thread freeing a Spoor mid-read — is invisible here precisely because the
model's steps are atomic by construction. Prose plus [[gate-smp]] carry that
half.

**Two of `CoveredIsItsPoint`'s three clauses hold by construction.** At most
one covered member, and it is the point's own directory: `CovMember` is the
only builder, it names `Covered[pt]`, and the clean model calls it only at a
fresh point. `BUGGY_COVERED_TAKES_FLAGS` fails the third clause (never MBEFORE
or MCREATE); the kernel test `territory_mount.union_keeps_covered` pins all
three at runtime. (The buggy cfg was added at holotype round 3's close of B-1d,
when pairing each buggy cfg with its invariant showed this one had none.)
