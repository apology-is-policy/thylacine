# AS-R9 integration manifest

Written for astra's review item R1 (yip 0161). Every figure below is DERIVED by
the script that generated this file, not typed from memory: regenerate with
`sh work/oct5-as-r9/make-manifest.sh` after any commit.

- branch: corona/async-memory
- base:   5ff62b788  (equal to astra's HEAD at review time -- asserted by the
  runbook's stage 1, which refuses when her HEAD moves off this base)
- tip:    5e09e801168c8b82dd444a0b850f8a97a16806ee  (the commit this manifest was GENERATED AGAINST; the
  manifest's own commit sits above it, so regenerate rather than reading
  this line as HEAD)
- commits in range: 74
- nothing pushed; nothing landed on main

## EXCLUDED FROM DELIVERY -- local configuration, not implementation

Commit `55cfdb54c` is EXCLUDED. It touches exactly one path:

    .claude/settings.json

It is operational hook configuration for this checkout (stale yip hook entries
naming a path the installer moved). Per astra's R1 it is NOT reverted locally --
reverting it would break this checkout's hooks to satisfy a packaging concern --
it is excluded from the integration set instead.

CONSEQUENCE AN INTEGRATOR MUST NOT MISS: the delivery is therefore NOT a
contiguous range. It is "every commit in base..tip EXCEPT that one", so a plain
`git merge` or a range cherry-pick would carry the config change in.

Verification that an assembled integration excludes it -- this must print nothing:

    git diff <base>..<integrated> --name-only -- .claude/

And on this branch, exactly one commit touches that path (so there is nothing
else of this class hiding in the range):

    $ git log --oneline 5ff62b788..5e09e801168c8b82dd444a0b850f8a97a16806ee -- .claude/
    55cfdb54c Drop stale yip hook entries from .claude/settings.json

## ASTRA'S FOUR PROTECTED WORKING DRAFTS

The four authority/settings drafts live only in astra's working tree and are
UNTOUCHED: this checkout has never contained them, which is checkable rather
than merely stated -- my base IS her HEAD, so anything of hers that is
uncommitted cannot appear in my range by construction.

R1 singles out test.c's added registrations, because BOTH of us append there.
My entire delta to kernel/test/test.c across the whole branch is additive and
consists of nothing but my own four tests -- four forward declarations and four
table rows:

    // AS-R9: settled drops + the JIT remaining-reference premise.
    void test_burrow_settled_drop_retains_nonfinal_charge(void);
    void test_burrow_settled_drop_exact_payer(void);
    void test_burrow_settled_mapping_drop_defers_free(void);
    void test_burrow_unmap_failure_leaves_mapping_attached(void);
    void test_burrow_unmap_interior_start_refused(void);
    void test_loom_private_owner_lifecycle(void);
        // AS-R9: the charge decision inside the drop's lock interval.
        { "burrow.settled_drop_retains_nonfinal_charge",
          test_burrow_settled_drop_retains_nonfinal_charge, false, NULL },
        { "burrow.settled_drop_exact_payer",
          test_burrow_settled_drop_exact_payer,             false, NULL },
        { "burrow.settled_mapping_drop_defers_free",
          test_burrow_settled_mapping_drop_defers_free,     false, NULL },
        { "burrow.unmap_failure_leaves_mapping_attached",
          test_burrow_unmap_failure_leaves_mapping_attached, false, NULL },
        { "burrow.unmap_interior_start_refused",
          test_burrow_unmap_interior_start_refused,         false, NULL },
        { "loom.private_owner_lifecycle",    test_loom_private_owner_lifecycle,    false, NULL },

So her draft registrations and mine are append-only into the same two regions
(the declaration block and the registration table) and do not overlap. The
integration of test.c is additive; the ORDER of the two sets inside those
regions is hers to resolve in her tree, and I have not pre-empted it.

## WHAT IS DELIVERED, BY CATEGORY

    kernel source (the repair)      : 12 file(s)
    kernel tests                   : 5 file(s)
    tools/ (SHARED SURFACE)        : 3 file(s)
    vault dossiers                 : 9 file(s)
    docs                           : 4 file(s)
    specs                          : 0 file(s)
    work/ evidence + runbooks      : 112 file(s)

The tools/ files are a shared surface main and aux also bake from. The one
behavioural change there is smp-multiboot.sh's SMP_KEEP_LOGS retention, which is
DEFAULT OFF, so no peer's gate changes unless they opt in.

## NOT DELIVERED, DELIBERATELY

- The green artifact set at work/oct5-as-r9/cpu1-green-pair/ is binary and stays
  UNTRACKED; only its MANIFEST.txt is committed. Nothing in the delivery asks an
  integrator to trust a binary I produced.
- Nothing generated from astra's build/ cache qualifies my source: the clone
  approval was spent and the qualifying run rebuilt from my own tree (provenance
  in work/oct5-as-r9/provenance.log).

## GATING STATE AT DELIVERY -- unchanged by this manifest

- ONE AXIS. The 50/50 clean boots and the D7 50/50 witnesses are Apple M2 + HVF
  only; the thyla-pi A72/KVM leg never ran (operator: mac gate alone, residual
  recorded). This is not two axes.
- The 128 MiB protection is retained; private async, replacement memory
  accounting and clipboard remain NON-DEFAULT and ungated by this work.
- The paused private-owner draft (base c822021a2ea56a452b4cdbe7709e6fa117a7678b)
  stays shut pending astra's review close.
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
