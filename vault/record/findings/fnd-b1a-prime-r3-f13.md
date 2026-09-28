---
id: fnd-b1a-prime-r3-f13
type: fnd
title: "A read fault queued behind a peer thread's copy-on-write break asks the read arm for a read-only install over the writable leaf the break left; the install refuses the mismatch and the Proc is terminated for reading a page it holds"
round: adt-b1a-prime-r3
severity: P1
status: fixed
surface: [sub-kernel-fault, sub-kernel-mmu]
threatens: [inv-i44, inv-i32]
fixed-by: chg-2026-09-23-b1a-prime-close-r3
regression: "cow.read_queued_behind_break_is_handled"
created: 2026-09-23
---
## Prosecution

After a fork every leaf of the parent's COW ranges is cleared, so every
thread's next touch faults. Two threads of the forked Proc touch the same
page: T1 writes, T2 reads. Both enter `userland_demand_page` and serialize
on `as->lock`. T1 wins: the write arm breaks and step 5 installs a WRITABLE
leaf. T2 then runs `demand_page_locked` with a fault decoded when the leaf
was invalid: ANON_LAZY resident hit, `VMA_FLAG_COW`, not a write ->
`install_prot = vma->prot & ~VMA_PROT_WRITE` -> `mmu_install_user_pte_attr`
finds the RW leaf, wants RO, `existing != want`, returns -1 -> `rc < 0` ->
`FAULT_UNHANDLED_USER` -> `proc_fault_terminate(NOTE_NAME_SNARE_SEGV)`. The
Proc dies for reading a page it holds. The same shape reaches a read that
faulted between the replace's invalid write and its new leaf. The
idempotent-install proof in `mmu.c` named "matching PA + prot"; the read arm
is the one arm that installs a prot NARROWER than the leaf a sibling may
have left. Pre-existing since L-4b -- but in the exact function the F9 fix
rewrote, and realistic: any multi-threaded program that forks has every
thread re-touching the heap after the fork.

## Fix

The check Linux's fault path makes under the lock: step 2b of
`demand_page_locked`, after the VMA / prot admission and before any arm,
asks `mmu_user_pte_admits(as, page_va, fi->is_write, fi->is_instruction)` --
a non-growing read of the leaf (VALID, AP[1] set for EL0, AP[2] clear when a
write, UXN clear when an instruction fetch) -- and returns `FAULT_HANDLED`
when a leaf already admits the access: nothing charged, nothing installed,
the instruction retried. Every arm, so no arm can narrow itself into a
refusal; the install's -1 on a mismatch stays, unreachable from this path.
The regression test drives both break shapes (the child's copy, then the
parent's take-in-place) with a read decoded before the break and run after
it; the two older break tests end with the same read. RED `nopeercheck`
(the pre-check removed) reddens all three.
