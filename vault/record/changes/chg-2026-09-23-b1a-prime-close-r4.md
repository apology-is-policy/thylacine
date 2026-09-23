---
id: chg-2026-09-23-b1a-prime-close-r4
type: chg
title: "B-1a' (capacity), the round-4 close: the pager refuses the abort classes it cannot resolve, the pool's refusal is ENOMEM on the exec frame, the reclaim asks for the shortfall, the probe fires at the leaf write, cow.tla models the read-only leaf -- round 4's five findings closed, no round 5 owed"
date: 2026-09-23
arc: arc-boosty
commits: ["387ffcd8"]
touched:
  - sub-kernel-mm-phys
  - sub-kernel-mmu
  - sub-kernel-addrspace
  - sub-kernel-fault
  - sub-kernel-pagemap
  - sub-kernel-burrow
  - sub-kernel-image
  - sub-kernel-vma
  - sub-kernel-devproc
  - sub-kernel-loom
  - sub-kernel-exec
  - sub-kernel-syscall-dispatch
  - sub-netd-server
  - sub-kernel-protect-witness
  - moc-kernel-memory
  - inv-i32
  - inv-i44
  - spec-capacity
  - spec-cow
established: []
closed: []
opened: []
supersedes: chg-2026-09-23-b1a-prime-close-r3
no-dossier-change: "the round-4 close changed no code in the syscall dispatcher or netd; the touched list carries the chunk's whole set from the superseded note, whose dossiers were co-staged at their own closes"
depth: rich
created: 2026-09-23
---
## What the superseded note said, and what changed

[[chg-2026-09-23-b1a-prime-close-r3]] records the chunk through the round-3
close -- the copy keeping its share until its leaf is replaced, a fault
answered by a leaf that already admits it, the bounded strip, ENOMEM inside
exec -- and says round 4 would run on those fixes and be appended at the
landing. It ran ([[adt-b1a-prime-r4]]), re-derived the five closes sound, and
found one defect older than the chunk hiding behind the pre-check round 3
added.

## The round-4 close (WIP 11 on the branch)

[[fnd-b1a-prime-r4-f17]] [P1]: an EL0 abort that is neither a translation,
an access-flag nor a permission fault -- an alignment fault or a synchronous
external abort, raised on a MAPPED page by the instruction itself -- reached
the pager and was answered `FAULT_HANDLED`, so the ERET re-executed the
instruction into the same abort forever. As built: `fault_info` decodes
`is_alignment` / `is_external` and the top of `userland_demand_page` refuses
the class before any lookup, `FAULT_USER_BUS` -> `snare:bus`
([[sub-kernel-fault]]); `demand_page.alignment_abort_is_bus_not_handled`
and `/bus-probe-child` (a misaligned `ldxr` across a 16-byte boundary, the
shape that faults under FEAT_LSE2's relaxed rule for ordered accesses as
well as ARMv8.0's) are the witnesses.

F18 [P3]: the round-3 ENOMEM claim held for the two mappers' allocation arms
only. `vma_insert_in`'s cap refusal is `-T_E_NOMEM` ([[sub-kernel-vma]]),
`burrow_map_in` propagates it ([[sub-kernel-burrow]]), and the stack, its
guard, `map_file_backed` and `exec_build_init_stack` (`int *err_out`) carry
it to the load's callers ([[sub-kernel-exec]]);
`execve.load_refuses_nomem_on_the_frame`.

F19 [P3]: `alloc_user_pages` asks the reclaim for `pool_shortfall(n)` -- the
pages that have to leave the pool for the charge to land, never 0 -- so an
exempt overshoot is folded into one request instead of one full cache scan
per page under the faulter's lock ([[sub-kernel-mm-phys]]);
`demand_page.reclaim_asks_for_the_shortfall`.

F20 [P3]: the F12 probe fires at step 5, immediately before the replace, and
two witnesses cover the exits it could not see:
`cow.break_copy_releases_the_share_on_a_failed_replace` and
`cow.break_copy_last_share_frees_after_the_replace` ([[sub-kernel-fault]]).

F21 [P3]: `cow.tla` extended behind `MODEL_LEAF` ([[spec-cow]]): `pter[s]`
(a read-only leaf naming the pristine page), `BreakReplace` then
`BreakRelease` in place of the one-step `BreakFinish`, bug 7
`BUGGY_PUT_BEFORE_REPLACE`, the invariants `NoReadableFreed` and
`NoCrossSpaceRead`; `cow_leaf` clean at 2996 states, pinned;
`cow_buggy_put_before_replace` fails on F12's exact chain; the eight older
cfgs reproduce their counts with the switch off. SPEC-TO-CODE and the spec
note state the order as built ([[inv-i44]]).

Three REDs joined the harness (nobusgate, nostackerr, noshortfall; sixteen
in all). `docs/ERRORS.md`'s `snare:bus` row, which still read "RESERVED --
no v1.0 emitter" though REVENANT had made it an emitter, is corrected --
a scripture note for the operator, the name and value unchanged.

## Verification (as recorded by the close, re-taken on the final tree)

The kernel suite 1664/1664 at `-smp 4` and at `-smp 1` on WIP 11 (five new
tests), joey clean (`/bus-probe-child ok`, CL-5 OK, `capacity-probe: ALL OK`
+ reaped, net-8a PASS); `specs/check-cow.sh` ALL CFGS AS CLAIMED (ten).
The sixteen REDs and the SMP gate run on the final tree.
