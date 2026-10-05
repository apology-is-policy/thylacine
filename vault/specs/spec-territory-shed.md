---
id: spec-territory-shed
type: spec
title: "territory_shed.tla"
models: [sub-kernel-territory]
pins: []
cfgs:
  - "territory_shed.cfg -- clean: ShedLosesNothing, WalkerWithinClosure and NoResidueAfterPivot at 3 trees x 3 points, no per-walker Dev (744,864 distinct)"
  - "territory_shed_perwalker.cfg -- clean, PerWalker = {t2, t3}: two trees of a Dev that stamps the walker's own devno (793,408 distinct)"
  - "territory_shed_buggy_nontransitive.cfg -- BUGGY_SHED_NONTRANSITIVE: keep only the entries keyed in the seeds (ShedLosesNothing)"
  - "territory_shed_buggy_no_union_seed.cfg -- BUGGY_SHED_NO_UNION_SEED: forget the union root's mount point (ShedLosesNothing)"
  - "territory_shed_buggy_undeclared_per_walker.cfg -- BUGGY_SHED_UNDECLARED_PER_WALKER: match a per-walker Dev on devno (ShedLosesNothing)"
  - "territory_shed_buggy_dotdot_escapes.cfg -- BUGGY_RESOLVER_DOTDOT_ESCAPES: a resolver that goes up, the premise broken (ShedLosesNothing)"
  - "territory_shed_buggy_keeps_all.cfg -- BUGGY_SHED_KEEPS_ALL: the pre-fix kernel, which sheds nothing (NoResidueAfterPivot)"
gate: "any change to the shed's closure, its seeds, Dev.devno_per_walker, or the resolver's consults of a union's mount point"
created: 2026-10-05
updated: 2026-10-05
---
## Abstraction

A namespace swaps its root (pivot_root or chroot), and the mount entries whose
mount points can no longer be reached from the new root must go. Nothing can
name them for an unmount, and every child would inherit them, so their slots
would be lost for the life of the namespace. The model is the shed that runs
under the same lock hold as the swap: which entries it keeps, and whether a
resolution from the new root still crosses every mount it crossed before.

The module keeps two definitions apart on purpose. Its first version stated
soundness against the same closure the shed was built from, so any rule passed,
including keeping only the root and keeping everything. The ground truth is now
an operational walker that shares nothing with the rule. It starts where the
resolver starts (the root's tree, and the union's mount point when the root is
a union handle), crosses a mount whose point lives in the tree it stands in,
may land in any tree of a Dev that stamps the walker's own devno, and never goes
up.

## What it pins

- **`ShedLosesNothing`** — wherever the walker stands, every entry of the
  pre-shed table that fires there survived. By induction along the walk, a
  resolution from the new root crosses exactly the mounts it crossed before the
  shed. Four buggy cfgs fail it, each through a different way to keep too
  little.
- **`NoResidueAfterPivot`** — right after a pivot, every surviving entry is
  keyed in a reachable tree: the slots come back. The pre-fix kernel, which shed
  nothing, fails it.
- **`WalkerWithinClosure`** — the two truth-side definitions agree. It is a
  consistency check rather than an independent pin; it names which definition
  moved when one does.

The rule is per tree and conservative, because the kernel cannot see inside a
9P tree: it may keep an entry no walk fires, never drop one a walk can.

## What it cannot see

A seed missing from both the rule and the truth. If the walker forgot the union
point together with the shed, the no-union-seed cfg would report no error. That
guard is prose: the audit-trigger row's item and the comments at the resolver's
two consults of the union's mount point.

Also beneath the model: reference counts (each dropped entry releases what an
unmount releases; the kernel tests and the audit cover it), a walk relative to a
directory descriptor opened before the swap (an accepted observable change),
qid paths (the rule is per device instance), and a dissolved union held outside
the root, whose rule lives in ARCH 9.6.10 and the resolver.

The downward-only walk is a recorded premise, not a belief: the dotdot-escapes
cfg is a resolver that goes up, and the rule is unsound against it. Path
containment (I-28) is what keeps the premise true.

## Binding

`specs/SPEC-TO-CODE.md::territory_shed.tla`. The pivot ↔ `territory_pivot_root`
and `territory_chroot`, each running `territory_shed_unreachable_locked` under
one `ns_lock` hold; the rule's seeds, per-walker widening and fixpoint ↔ the
closure loop in `territory_shed_unreachable_locked`; the walker ↔ `stalk_core`
and `devenv_walk`'s per-caller devno stamp. `specs/check-territory-shed.sh`
runs all seven cfgs, pins the two clean counts and checks which invariant each
buggy cfg violates.
