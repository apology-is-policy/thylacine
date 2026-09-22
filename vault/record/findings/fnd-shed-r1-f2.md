---
id: fnd-shed-r1-f2
type: fnd
title: "ShedLosesNothing is a tautology: the spec cannot detect a wrong reachability rule"
round: adt-shed-r1
severity: P2
status: fixed
surface: [sub-kernel-territory]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "three sabotages of the rule's closure in scratch copies each fail (too small -> ShedLosesNothing; every tree -> NoResidueAfterPivot; the truth too small -> NoResidueAfterPivot); the five territory_shed_buggy_* cfgs each violate their OWN invariant"
created: 2026-09-21
---
## Prosecution

**File**: `specs/territory_shed.tla` (first version)
**Invariant**: the spec-level rigor claimed for ARCH 9.6.10
**Prosecution**:
1. The clean `Keep` is `{ m \in M : Home[m[1]] \in Reach(new, M) }`; `lost'` adds `{ m \in mounts \ Keep(new, mounts) : Home[m[1]] \in Reach(new, mounts) }`.
2. Those are complements under the same predicate, so `lost = {}` holds for ANY `Reach`.
3. Measured on scratch copies, clean cfg: `Reach(r, M) == {r}` (the rule the `nontransitive` cfg exists to reject) -> no error, 5,832 states; `Reach(r, M) == Trees` (the pre-fix kernel) -> no error, 10,368 states.
4. The buggy cfgs detect a mismatch between `Keep` and `Reach`, not unsoundness of `Reach`. The module has no walk, no `..`, no fd, no union, no per-walker stamp -- F1 lives exactly there. It also excluded same-tree binds and same-tree root swaps, the commonest real cases.
**Suggested fix**: define walk-reachability independently of `Keep` and state the invariant against it.

## Disposition

Fixed by a rewrite: the ground truth is an operational WALKER (start in the root's tree and, for a union root, the point's; cross; per-walker restamp; never up) that shares NO operator with the closure the rule computes; `WalkerWithinClosure` pins the truth from below. Same-tree binds and swaps are explored. Three new buggy cfgs (`no_union_seed` = F1, `undeclared_per_walker`, `dotdot_escapes` = the I-28 premise as an executable dependency). One limit is stated in the module: a truth closure that is too LARGE only weakens the completeness half, and nothing pins it from above.
