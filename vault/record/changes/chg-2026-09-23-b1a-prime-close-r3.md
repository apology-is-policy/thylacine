---
id: chg-2026-09-23-b1a-prime-close-r3
type: chg
title: "B-1a' (capacity), the round-3 close: the copy-on-write copy keeps its share until its leaf is replaced, a fault that finds a leaf already admitting its access is answered by it, the strip takes what is wanted, a refused allocation inside exec is ENOMEM -- round 3's five findings closed, round 4 on the fixes"
date: 2026-09-23
arc: arc-boosty
commits: ["*(pending)*"]
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
  - spec-capacity
established: []
closed: []
opened: []
supersedes: chg-2026-09-23-b1a-prime-close-r2
depth: rich
created: 2026-09-23
---
## What the superseded note said, and what changed

[[chg-2026-09-23-b1a-prime-close-r2]] records the chunk through the round-2
close -- the pool reclaiming idle images before it refuses, a mapped FILE
page charged to its holder per leaf, the copy-on-write break replacing its
leaf in place, a fork keeping the parent's tables -- and says round 3 would
run on those fixes. It ran ([[adt-b1a-prime-r3]]) and found that the leaf
replace had voided the argument the old uninstall carried.

## The round-3 close (WIP 9 on the branch)

[[fnd-b1a-prime-r3-f12]] [P1]: the copy branch put its share of the original
right after the slot swap, while the space's own read-only leaf still
translated to it and the replace at step 5 was the first point that
translation died -- the other holder, sole, could take the page in place and
write into what a sibling thread still read, or exit and free it under a
live leaf. A regression of the F9 fix. As built: `cow_release` carries the
original past step 5 and the put (with the free on the last share) runs
after the replace, on the failed-replace path too ([[sub-kernel-fault]]); a
tests-only probe between the swap and the replace lets
`cow.break_copy_pins_the_original_until_replaced` assert the stale leaf as
the control and the held share as the claim.

[[fnd-b1a-prime-r3-f13]] [P1]: a read queued behind a peer's break asked the
read arm for a read-only install over the writable leaf, and the mismatch
refusal terminated the Proc. As built: step 2b of `demand_page_locked` asks
`mmu_user_pte_admits(as, va, write, exec)` ([[sub-kernel-mmu]]: a non-growing
read of the leaf -- EL0 access, AP[2] clear for a write, UXN clear for an
instruction fetch) and answers `FAULT_HANDLED` when a leaf already admits
the access, for every arm, before any charge; the install's refusal of a
mismatch stays, unreachable from the fault path.
`cow.read_queued_behind_break_is_handled` and a read leg in each older break
test.

F14 [P3]: `burrow_image_strip(v, want)` stops at the pages wanted, lowest
slots first; `image_cache_reclaim` passes `want - freed`
([[sub-kernel-burrow]], [[sub-kernel-image]]). The finding's second half --
give up when the pool is still over-full after a reclaim -- is declined: an
exempt overshoot is the reserve's designed case and the bar forbids refusing
a user while idle cache pages exist; bounded, the loop frees the overshoot
plus the request once. `demand_page.idle_image_reclaimed_under_pressure`
strips a two-page image in two allocations and carries the physical control
deferred from round 2: with the magazines drained the buddy gains exactly
what the pool released.

F15 [P3]: a pool refusal inside the load surfaced as EINVAL. Both segment
mappers return `-T_E_NOMEM` from their allocation arms, the body passes it
through, `sys_execve_core` reports it ([[sub-kernel-exec]],
[[sub-kernel-syscall-dispatch]]); `execve.load_refuses_nomem_at_the_pool_edge`.

F16 [P3]: the F9 witness was satisfied by the defect it named; both break
tests now pin the L3 table's PA and `mmu_uninstall_pte_calls()` unchanged
across the break; `demand_page.file_pages_charge_the_holder` detaches the
unfaulted half of a re-mapped image first (nothing refunded) and then the
faulted one (one refund).

Four REDs joined the harness (nocowpin, nopeercheck, nowantstrip,
noexecnomem; thirteen in all).

## Round 4

Round 4 (Fable 5.1) ran on the round-3 fixes, scoped to them; its record is
appended at the landing.

## Verification (as recorded by the close, re-taken on the final tree)

The kernel suite 1659/1659 at `-smp 4` and at `-smp 1` on WIP 9 (three new
tests), joey clean (CL-5 OK, `capacity-probe: ALL OK` + reaped, net-8a
PASS). The thirteen REDs and the SMP gate run on the final tree.
