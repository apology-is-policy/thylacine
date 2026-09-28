---
id: adt-b1a-prime-r4
type: adt
title: "B-1a' (capacity) round 4: an EL0 abort the pager cannot resolve was answered as handled and re-faulted forever; the five round-3 closes re-derived"
date: 2026-09-23
scope: [sub-kernel-fault, sub-kernel-mmu, sub-kernel-image, sub-kernel-burrow, sub-kernel-exec, sub-kernel-vma, sub-kernel-mm-phys, spec-cow, inv-i32, inv-i44]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 4}
findings: [fnd-b1a-prime-r4-f17]
round-of: chg-2026-09-23-b1a-prime-close-r3
created: 2026-09-23
---
## Scope

Branch `b1a-prime-wip` at 25cb0d5b (WIP 9, the round-3 fixes: `cow_release`
put after the replace, step 2b `mmu_user_pte_admits`, the bounded strip,
`-T_E_NOMEM` through exec's mappers, the discriminating witnesses). Read-only.
The brief asked for the five closes re-derived from the code: the pre-check's
admission on every arm (a valid leaf that admits the access but names a page
the slot no longer holds), the share's release on every exit of the copy
branch, uaccess under a held Burrow lock against the reclaim's order, the
bounded strip's cursor under a partially stripped image, the errno sweep,
the tests as witnesses.

## Verdict

0 P0 / 1 P1 / 0 P2 / 4 P3; clean by the count rule and by the shape of the
fixes (a class gate, an errno path, a request size, a probe's position, a
model extension -- none invasive), so no round 5 is owed. F17 is
pre-existing (since P3-Dc) and hid behind the pre-check round 3 added: the
one site every arm now passes through is also where the class is refused.
All five round-3 closes re-derived sound: F12's pin holds on every exit
(the failed-replace put is safe because no -1 path leaves a valid leaf),
F13's pre-check is sound for every arm on the three classes it was built
for, F14's bound and termination hold, F15 held for the mappers' allocation
arms only (F18), F16's pins discriminate. Withdrawn with a named guard: a
leaf naming a page the slot no longer holds (every slot change clears the
range's leaves first; the COW window is the F12 pin), hardware faults on a
leaf the probe calls admitting, the uaccess path (PAN unconfigured -- the
trap is recorded in the trigger row), the FILE charge against 2b, an XN leaf
under an EXEC VMA, the share pin's exits, the bounded strip, the errno, and
uaccess under a Burrow lock (a second census, 176 sites, the same one hit).

## Findings

- [[fnd-b1a-prime-r4-f17]] [P1] an alignment or external abort answered as
  handled (fixed).
- F18 [P3] the F15 claim over-broad: `burrow_map_in` returned only -1 and the
  frame builder's refusal was EINVAL: fixed (`-T_E_NOMEM` through
  `vma_insert_in`'s cap, `burrow_map_in`, the stack, its guard,
  `map_file_backed` and `exec_build_init_stack`'s `err_out`).
- F19 [P3] an exempt overshoot cost one full image-cache scan per page, under
  the faulter's `as->lock`: fixed (`pool_shortfall(n)`, one request).
- F20 [P3] the F12 probe fired before step 5, so a put between the probe and
  the replace would have passed; the failed-replace put and the last-share
  free unwitnessed: fixed (the probe immediately before the replace; two
  witnesses).
- F21 [P3] `cow.tla` and SPEC-TO-CODE described the pre-F12 order and had no
  read-only-leaf state: fixed (the `MODEL_LEAF` extension; `cow_leaf` 2996
  states; `cow_buggy_put_before_replace` on F12's chain).

The close is [[chg-2026-09-23-b1a-prime-close-r4]].
