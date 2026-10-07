# Merge-time obligations -- HAND-WRITTEN, NEVER GENERATED

make-manifest.sh INCLUDES this file verbatim into INTEGRATION-MANIFEST.md's
gating section and REFUSES if it is missing. It lives apart because the manifest
is generated: obligations (3) and (4), including main's final loom.h contract
text recorded verbatim off yip 0183, were silently DESTROYED by a regeneration on
2026-10-07 -- by the very command the manifest's own header tells the reader to
run. Recovered from git and moved here, where no script rewrites them.

Add obligations HERE. Each one names what must be done at merge, against which
of main's commits, and how it was verified -- never "see the call", because a
call transcript is not in the delivery.

## STATUS 2026-10-07 ~13:3xZ: ALL FOUR NOW TARGET CODE ON MAIN

main moved from cb7194c10 to **25ed27f21** (18 commits) while this branch sat.
Every commit the obligations below are written against is now an ancestor of
main, tested one by one with `git merge-base --is-ancestor` rather than read off
a log:

    ON MAIN  f6f4c0397  loom-mc                      -> obligation (1)
    ON MAIN  cb7194c10  tag pool: land               -> obligation (2)
    ON MAIN  d8b177156  loomwb r1 close (the loom.h contract text) -> obligation (3)
    ON MAIN  ef64e4b3a  loomwb r2 close              -> obligation (3)
    ON MAIN  25ed27f21  exit close: land             -> obligation (4)

So obligations (3) and (4), recorded when they were still main's side branches,
are no longer anticipating anything: they are reconciliation work against
main's own history. NOTHING IS APPLIED and main is NOT merged -- that constraint
stands, and the loom.h contract text below is still taken AT MERGE, not now.
Obligation (4)'s anchor report was measured against the exit-close BRANCH; the
landed commit may differ from what was reviewed there, so re-measure the anchors
against 25ed27f21 at merge rather than inherit the branch's numbers.

VERIFIED SEPARATELY, because "main moved" and "something of mine landed" are
different claims: 0 of this branch's 85 commits are reachable from main.

- MERGE-TIME OBLIGATIONS against main, which this base cannot carry: (1) merging
  main f6f4c0397 (loom-mc) requires `loom_drive_moved_locked(l);` before the
  spin_unlock in loom_post_pool_cqe; (2) main's tag-pool changes loom.c's CQ pump
  budget to `submitted + P9_TAG_LIMIT + 1` -- verified independently as touching
  no charge-settlement path and no v->lock, so it is a reconciliation item and
  not a correctness interaction. (3) main's Loom write-behind fix (branch
  loomwb, sent on yip 0183) inserts a `dev9p_loom_register(spoors[i])` flush
  loop into `loom_register_handles`, which is where this branch's private-owner
  refusal sits. ORDER AT MERGE: magic check, private-owner refusal, n/arg
  checks, the Loom-4c SQPOLL deadline-capable gate, THEN their flush loop.
  main's own reason -- a refused owner should not pay a flush -- applies equally
  to a refused SPOOR, and this branch's SQPOLL gate rejects spoors that their
  loop would otherwise have already flushed and stopped staging. Raised with
  main on 0183 turn 2 and AGREED there; main's Fable round found the same prefix
  side effect independently (its F3) and the decision is to DOCUMENT it and NOT
  restore staging -- staging is a performance property, the caller usually
  retries, and restoring would reopen a stage window between flush and restore
  that the single lock hold avoids. The FINAL loom.h contract comment to take at
  merge (main 0183 note 1, loomwb d8b177156) reads "on failure (n out of range,
  or a dev9p Spoor's write-behind flush failed or had latched an error:
  dev9p_loom_register, which may wait) the caller retains its refs and the old
  table stands, though the Spoors before the failing one stay flushed and no
  longer stage (a cost only)". loom.c is unchanged from the hunk already
  recorded; everything else in their fold is dev9p.c/h, tests and docs.
  (4) their exit-close hunks: the loom_free join hunk
  applies here (4/4 anchors unique) but three anchors do NOT exist on this base
  -- `struct loom_sqpoll_wait w` (no fan-in machinery here, so their
  `closes_never_wait` line needs re-placing), `poll_waiter_list_unregister(w->cq)`
  (loom_sqpoll_fanin_park absent), and thread.h's `cons_frozen_unwound` (so
  their two new bools insert after exit_close_active here, and their stated
  field offsets and sizeof are main's numbers, to be re-measured not inherited).

## ADDED 2026-10-07 ~15:1xZ, from main's call 0192 (B-2a, branch b2 @212f8e479)

(5) B-2a makes BURROW_TYPE_CODE lazy: `burrow_create_code` loses its `exempt`
parameter and takes ANON_LAZY's sparse pagemap, and CODE moves to the ANON_LAZY
arm of burrow_free_internal, burrow_acquire_mapping, burrow_lazy_resident_count,
burrow_lazy_footprint and burrow_lazy_slot_for_test. It lands together with B-2b.
AT MERGE: b2's syscall.c JIT functions win WHOLE over main's. On b2, measured by
main and reported on 0192 t3, sys_jit_create_region charges NOTHING (no npages,
no burrow_backing_pages, no burrow_charge_record), SYS_JIT_DESTROY refunds
`burrow_lazy_footprint` under as->lock before the unmaps, the extinction
"SYS_JIT_DESTROY: charge record disagrees with the region's page count" is GONE,
and the commit arm charges once per page at fault.c:708 carrying
proc_resource_exempt itself (jit.charges_once_per_page pins it; their J1 RED
re-adding a create-time charge reddened 6 jit tests).
  - MY SIDE OF IT: the long AS-R9 comment block I added above
    SYS_JIT_DESTROY's claim/restore -- the one naming the three premises that
    make THAT caller sound while five others were migrated to the settled drops
    -- must be RE-READ against the footprint refund rather than carried over
    verbatim. The premises are unchanged (every failure return in
    burrow_unmap_reporting precedes that function's first mutation; both aliases
    live in p->as whose lock is held across the interval;
    burrow.unmap_failure_leaves_mapping_attached pins premise 1). The QUANTITY
    the line below them asserts is what changed.
  - TEXTUAL: expect one conflict at burrow_acquire_mapping's tail. My hunks
    start AT its closing brace (@@ -917,24 +909,15 @@, then -945, then the charge
    machinery at -997/-1013/-1049); their last edit there is the ANON_LAZY arm at
    old ~902-911. Mine are below the type switch, theirs inside it: take both.
    No hunk of mine touches burrow_create_code or either free_internal arm.
  - CORRECTION TO MY OWN FIRST READING, kept because it is the kind of mistake
    that repeats: I measured main 25ed27f21 for the syscall side when the call's
    subject was b2 @212f8e479, and reported an eager JIT charge that b2 had
    already removed. The header of main's turn carried the right ref and I read
    the wrong one.

(6) SYSCALL NUMBER 127 IS A COLLISION, AND IT IS NOT MINE TO SETTLE.
`SYS_SRV_REGISTRY_NEW = 127` (syscall.h:2431) is ASTRA's, added by 417c8caeb
("Isolate login service registries and retain session connection budgets") inside
25ed27f21..5ff62b788. It is in my BASE because my base IS her HEAD. Measured:
`git diff --stat 5ff62b788..HEAD -- kernel/include/thylacine/syscall.h` is EMPTY
-- this branch mints no syscall number at all. B-2b adds SYS_JIT_CREATE_SEALED
and wants 127, the last number below vivarium's restart_syscall(128) ceiling
argument; precedent #50 (09-03) says whichever side lands SECOND renumbers, and a
move to 128 owes vivarium a per-number argument for restart_syscall.
AT MERGE: whoever integrates must know that my delivery cannot land before
astra's base does (`git merge-base --is-ancestor 5ff62b788 25ed27f21` -> NO; 86
commits of her base are not on main), so B-2 is ahead of this branch by
construction and main has been told to take 127. The renumber decision belongs to
astra; raised with her rather than answered on her behalf. NOT renumbering now is
deliberate: a syscall enum change rebuilds the kernel and voids the qualified
artifact this checkpoint's verdict names (5ced18c43ae8302a / e266c931d9668a44).
