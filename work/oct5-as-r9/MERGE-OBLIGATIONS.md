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
