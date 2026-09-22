---
id: fnd-b0self-r1-f2
type: fnd
title: "'..' popped a CROSSED base: '../x' off a mounted-over base read under the mount that 'x' read over"
round: adt-b0self-r1
severity: P2
status: fixed
surface: [sub-kernel-stalk]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "stalk.dotdot_crossed_base_floor (um1 over `a`; controls, '../shared', '../b', a './../.././' run, bare '..', leak); sabotage `floor` fails it at \"'..' at a crossed base is a no-op: still the mount\""
created: 2026-09-21
---
## Prosecution

**File**: `kernel/stalk.c`, the `..` arm: `if (depth > 0) spoor_clunk(trail[--depth]);`
**Invariant**: I-28
**Prosecution**:
1. A base that is itself a mount point crosses BEFORE the component loop; its mounted root is pushed as `trail[0]`.
2. `".."` at the bottom popped that entry: `depth == 0`, and the position is the uncrossed `base` -- the directory the mount covers.
3. The next real component took `parent = base`. The comment there ("already proven not-a-mount by the base cross") was true only until such a pop.
4. So `"../x"` resolved `x` in the covered directory while `"x"` resolved it in the mounted tree. Pre-existing; found by reading the arm.

## Disposition

Fixed @d5c58d76: `floor_depth` is set at the base cross on every pass (a symlink restart re-crosses) and the arm pops only above it. A bare `".."` then yields `trail[0]`, the same Spoor `"."` already yielded. ARCH 9.6.7's containment bullet, the stalk audit row and the dossier state the floor.
