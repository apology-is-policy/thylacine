---
id: chg-2026-10-12-loom-setup-ref-and-settle
type: chg
title: "Loom setup holds its own reference across the install; the rollback settles"
date: 2026-10-12
arc: arc-boosty
commits: ["3f4896413"]
touched:
  - sub-kernel-loom
  - sub-kernel-syscall-dispatch
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Found, fixed and audited by Corona on corona/async-memory (6a5039788,
guest-verified 3f816c8a4); ported onto main cc422b913 as 3f4896413 and landed
by main as a fast-forward.

**What** ([[sub-kernel-loom]], [[sub-kernel-syscall-dispatch]]). Two defects
in the ordinary `SYS_LOOM_SETUP` path. (1) P1, latent, EL0-reachable kernel
UAF write: `sys_loom_setup_for_proc` wrote the owner fields, the ring's charge
record and every out field through the Loom after `handle_alloc` had adopted
its only reference, so a sibling thread's close of the new fd in that window
freed the Loom under those stores. Setup now takes its own reference before
the install (both are dropped if the install fails) and drops it as its last
act. (2) P3, I-32, self-only: the params-copyout-fault rollback ran
`handle_close` and then `burrow_unmap`, the ring's final drop, which has no
payer, so the charge record died unrefunded on every attempt.
`sys_loom_setup_rollback` closes and then detaches through the settling path,
and whichever drop ends the ring claims the record.

**Witnesses.** `loom.setup_close_race` closes the fd from a KERNEL_TESTS hook
at the install point and samples `loom_total_destroyed`, a monotonic counter,
at the close. `loom.setup_rollback_settles` checks that the rollback refunds
the ring's charge exactly.

**Verification.** Corona: NA-1 audit round 1 (Fable 5.1) filed nothing against
either fix; F2 [P3] (the rollback's fd and VA reusable by a sibling) closed
with a reason, a self-race in which every ledger settles once. Main:
Port 3f4896413 on main cc422b913: suite 1977/1977 (main's 1975 plus loom.setup_close_race and loom.setup_rollback_settles). Corona's mutants, re-taken in main's tree by name (na1-mutants.py): M4 (no own reference) fails exactly loom.setup_close_race, at "the Loom outlives a close that lands at install"; M5 (burrow_unmap in the rollback) fails exactly loom.setup_rollback_settles, at "the rollback settles the ring's charge exactly" (1976/1977 each), as Corona predicted. test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, ubsan smp4/8, 0 corruption); check-production --all PASS (the KERNEL_TESTS=OFF shape, where the new test hook compiles out, builds and boots).
