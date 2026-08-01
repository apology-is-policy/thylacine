---
id: spec-territory
type: spec
title: "territory.tla"
models: [sub-kernel-territory]
pins: [inv-i1, inv-i3]
cfgs:
  - "territory.cfg -- clean (TypeOk + NoCycle + MountRefcountConsistency + MountRefcountNonNegative)"
  - "territory_buggy.cfg -- BUGGY_CYCLE: bind without the cycle check; two binds compose into a loop (NoCycle)"
  - "territory_buggy_mount_no_refbump.cfg -- mount adds the entry, skips the ref bump (MountRefcountConsistency)"
  - "territory_buggy_unmount_no_refdrop.cfg -- unmount removes the entry, skips the ref drop (leak)"
  - "territory_buggy_destroy_leak.cfg -- final release clears mounts[] without dropping refs"
  - "territory_buggy_chroot_no_refbump.cfg -- chroot stamps root_spoor without the bump or the drop-of-old"
gate: "Pre-commit re-run for ANY change to a mount-table / root_spoor mutation site (mount, unmount, chroot, pivot_root, clone, final release). The four refcount cfgs ARE the miss-one-site regression net."
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

Namespaces are per-Proc function values over abstract paths; Spoors are
opaque names with a modeled refcount. The model's whole subject is
**bookkeeping**: it tracks who holds a reference and whether the counter
agrees with the set of holders. Deliberately beneath it: what a mount
POINT is (the model keys on an abstract `path`; the impl keys on the
Plan 9 `(dc, devno, qid.path)` triple since stalk-2), what crossing
does, the `mp_path` names, the cwd, and both locks.

`refcount` is modeled as a SEPARATE variable from the mount set — that
separation is the point. Keeping the counter independent of the
cardinality it should equal is what makes "forgot to bump" and "forgot
to drop" catchable as a desync rather than being true by construction.

## Action-site map

| Spec action | Impl |
|---|---|
| `Init` | `territory_init` / `territory_alloc` (via `territory_init_fields`) |
| `Bind` / `Unbind` | `bind` / `unbind` — the DEAD table (see below) |
| `Mount` | `mount`'s append arm (`spoor_ref` then install) |
| `Unmount` | `unmount`'s swap-remove + the deferred `spoor_clunk` |
| `Chroot` | `territory_chroot` AND `territory_pivot_root` — the same state transition under two preconditions |
| `ForkClone` | `territory_clone` (mount refs + root ref; the `mp_path` and `dot_path` copies are beneath the model) |
| `BuggyDestroyLeak` | the counterexample to `territory_unref`'s final-release loop |

MREPL has no distinct action — it is `Unmount` then `Mount` in one
atomic step at the impl, and the model's set semantics make the pair
indistinguishable from the composite.

## Known gaps

**`NoCycle` models the dead table.** The spec's only cycle invariant
ranges over `bindings`, and at v1.0 nothing populates `binds[]` — no
`SYS_BIND` exists and `bind()` has no production caller (see
[[sub-kernel-territory]] Caveats). The LIVE cycle risk is on the mount
identity graph, guarded by `would_create_mount_cycle` — added because
[[fnd-stalk2-r1-f1]] showed I-3 did NOT hold there "by construction" —
and that check has no model. Tracked as
[[seam-mount-graph-unmodeled]]; the impl-side protection is the
`territory_mount.rejects_cycle` test and the dossier's Prosecution list.

**Isolation is structural, not a state invariant.** [[inv-i1]] is
encoded by the data model — every action touches one Proc's slot — so a
buggy variant that updated two Procs in one step would need a temporal
property to catch. When RFNAMEG lands
([[seam-rfnameg-shared-territory]]) the sharing becomes real and
Isolation must become a checked invariant.

**Neither lock is modeled.** `ns_lock` and `dot_lock` serialize what the
model treats as atomic actions. The RW-4 SA-F1 race
([[fnd-rw4-sa-f1]]) — a peer thread freeing a Spoor mid-read — is
invisible here precisely because the model's steps are atomic by
construction. Prose plus [[gate-smp]] carry that half.
