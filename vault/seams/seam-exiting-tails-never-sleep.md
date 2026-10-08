---
id: seam-exiting-tails-never-sleep
type: seam
title: "EXITING tails must never SLEEP — a property held by accident, not enforcement"
status: open
surface: [sub-kernel-death]
opened-by: chg-2026-07-14-68-last-thread-out-close
tracker: "unfiled"
created: 2026-08-01
updated: 2026-10-08
---
## Owed

The #68 close window's soundness rests on a stated property: an EXITING
peer's residual execution never touches the handle table and never SLEEPS.
The second half is what is owed, because it is currently true by
COINCIDENCE rather than by construction.

An EXITING thread's remaining work is the clear-child-tid handoff plus
`sched()`. The handoff does `uaccess_store_u32` into a user VA — which
COULD demand-page. Sleeping while EXITING trips `sched()`'s "current is not
RUNNING" assertion, i.e. extincts.

It does not happen today because every writable VMA is eager or lazy-anon
(the lazy arm resolves fully under `vma_lock`, without blocking) and FILE
Burrows are never writable — the REVENANT dispatch gate keeps `PF_W`
segments eager.

The property rests on four sites, any one of which a future change could undo
(re-verified 2026-10-08 at the XT-3b audit, F4):

- `exec_load_into`'s `file_shareable` gate (`kernel/exec.c`): a segment is
  file-backed only if `PF_R` and not `PF_W`; a writable segment goes eager.
- `vivarium_mmap_file_decide` (`kernel/vivarium.c`): a FILE mmap asking for
  anything outside `VIV_MMAP_FILE_PROT_ADMITTED` is refused, and the DISTRO D-3
  FILE mmap arm (`kernel/syscall.c`) relies on that refusal ("PROT_WRITE cannot
  reach here").
- the VMA prot ceiling: `vma_alloc` sets it to the mint's prot, `burrow_protect`
  refuses a raise above it, and only the two anonymous mints raise it to RW. A
  FILE mapping minted without write can never gain it.
- the arms a writable fault can take are spin-only: the lazy-anon and COW arms
  run under `as->lock`, a `spin_lock_t`, and an allocation that finds the pool
  full reclaims through `image_cache_reclaim` under `g_image_lock`, never
  sleeping. The arm that DOES sleep is the FILE miss (`userland_demand_page`,
  reached from the EL1 uaccess fixup), which the three sites above keep away
  from every writable address.

XT-3b added two things that rest on it. `proc_drain_retired`, exec's wait for
retired tails, ends because every tail settles; and the bound of about twice
the CPU count on retired-but-allocated Threads assumes no tail sleeps. A tail
that slept would instead extinct in `sleep_common` ("current is not RUNNING"),
so neither turns into a hang. The other half of a tail's shape, that it is
never switched out involuntarily, is enforced since XT-3b by
`preempt_check_irq`'s refusal of an EXITING thread.

The tail is now only the clear-child-tid store, its wake and `sched()`, on both
exit paths. Until the XT-3b audit's round 2 (F3), `thread_exit_self`'s last
Thread out also ran the /srv, /cap and weft teardown after its EXITING commit;
whether those three slept rested on gates this note did not list (the weft
share admission keeping FILE Burrows out, srvconn's spin-only teardown). They
now run before the commit, while RUNNING and ALIVE, as `exits()` runs them, so
this property covers the store alone.

## What closes it

Either an explicit non-sleeping guarantee on the exit-tail uaccess path, or
an enforcement that catches the violation instead of relying on the mapping
taxonomy staying as it is.

The v1.x anon-COW / pageout work is the trigger: the moment a writable
mapping can fault into a BLOCKING arm, this property must be
re-established deliberately, before that work lands.

## Risk while open

None today. The failure mode when it breaks is an extinction on the exit
path — loud, but on the death lineage, which is where loud failures have
historically been hardest to attribute.
