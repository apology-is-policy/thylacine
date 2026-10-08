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

### (6) SETTLED BY ASTRA, 0161 t49 -- recorded here because my delivery carries her number

Her disposition, in her words and not my paraphrase of it: do NOT hold main's
B-2b 127 for astra; accept registry-number reconciliation at the later
coordinated base integration; no renumbering in the qualified checkpoint now. She
sent main the same disposition on 0116. So 127 belongs to SYS_JIT_CREATE_SEALED
and nothing is owed by this branch.

THE PART AN INTEGRATOR MUST NOT SIMPLIFY: the replacement is not blindly 128. It
resolves against the integration-time enum, and it travels with every consumer
plus vivarium's per-number restart_syscall 128 dispatch/isolation argument, the
sentinel/ceiling and the combined qualification -- together, in one fold. A
renumber that moves only the enum value leaves the ceiling argument asserting a
number that moved under it.

OWNERSHIP: this is ASTRA'S merge obligation. It is recorded on my list only
because my base IS her HEAD, so my delivery carries 417c8caeb and an integrator
reading this file would otherwise meet the collision with no pointer to its
owner. It is not new kernel scope here and it is not a landing authorization.

## ADDED 2026-10-07 ~21:0xZ, from main's call 0200 (B-2 LANDED on main 4b48cb0f6)

### 5. THE AddrSpace SIZE ASSERT COLLIDES. Both sides grew the struct and both
###    carry a drift alarm, with DIFFERENT expected values.

MINE (kernel/include/thylacine/addrspace.h:163, :166):
    u32            private_rings;
    _Static_assert(sizeof(struct AddrSpace) == 80,
                   "AddrSpace: existing layout plus owner/private-ring counts; "
                   "internal drift alarm, not a userspace ABI.");

MAIN @4b48cb0f6 (:153, :163):
    u32            code_vmas;
    u64            id;
    _Static_assert(sizeof(struct AddrSpace) == 72,
                   "AddrSpace is 72 bytes: ref+lock (8) + pgtable_root (8) + "
                   "context_id (8) + vmas (8) + the three I-32 u32 axes + "
                   "page_budget + page_peak + pgtable_pages + file_pages + "
                   "code_vmas (32) + id (8). "
                   "Growth is fine -- this assert is a drift alarm, not an ABI.");

main kept 72 by spending the FORMER PADDING on code_vmas; my branch grew to 80
for the owner/private-ring counts. So the merge inherits both new fields and two
asserts that cannot both be true.

AT MERGE, in this order:
  a. Keep ONE assert. Two will conflict textually; one kept blindly will be
     wrong whichever survives, and the wrong one fails the build LOUDLY, which
     is the good case -- the bad case is resolving the conflict by deleting the
     assert to make the build pass. Do not do that: it is the drift alarm for
     both features.
  b. DERIVE the merged number, never add it up by hand. Compile and read what
     sizeof reports (a one-file _Static_assert with a deliberately wrong value
     prints the actual size in the diagnostic). My own recorded lesson: a
     hand-counted 27-byte row was 26, and two hand counts are one reading.
  c. EXTEND main's enumeration rather than replacing it with a bare number.
     Their message names every field; the merged one must name private_rings
     and the owners count too, or the next person inherits a number with no
     derivation attached.

### 6. EVERY VMA RELINK MUST GO THROUGH vma_insert_in / vma_remove_in.
main's landing makes those two the only places code-Burrow aliases are counted
(code_vmas, under as->lock), and vma_remove_in EXTINCTS on underflow. At merge,
re-walk my branch for any VMA list manipulation that does not pass through them
-- the fixture's settling-detach path goes through the burrow/vma APIs, but that
is a claim to re-check against their final shape, not an assurance.

### 7. MY ENQUEUED vma_remove_in HARDENING ITEM MAY HAVE CHANGED SHAPE.
I enqueued a double-call head-wipe hazard in vma_remove_in (no reachable path;
main agreed on 0198). main's landing adds an EXTINCTION ON UNDERFLOW to that
same function. A second call would now trip that extinction rather than wiping
silently -- which, if it covers the hazard, turns a silent-corruption item into
a loud-failure one, and if it does not, leaves the item open with a new
neighbour. RE-EXAMINE against their landed code before touching it; do NOT
assume either way, and do not close the item on the strength of this note.
RE-EXAMINED 2026-10-08 against 4b48cb0f6:kernel/vma.c -- IT DOES NOT COVER IT.
The unlink (and with it the head-wipe on a second call) runs BEFORE the
code_vmas check. A double remove of a NON-code VMA still wipes as->vmas
silently. One of the LAST code alias extincts, but only after the wipe. One of a
code alias with others live SILENTLY UNDERCOUNTS code_vmas, which feeds the I-39
image join's CAP_JIT term -- a new neighbour, not a cover. Item stays OPEN, fix
shape unchanged (one linkage-keyed idempotence guard, with a double-remove
regression test). At merge, re-measure "no reachable double remove" over the
MERGED call-site set, main's B-2 sites included.

## ADDED 2026-10-08 08:41Z, from main's call 0202 (capmark LANDED, main = 060cbcc1f)

### 5 (UPDATED). The AddrSpace collision changed shape, not size.
capmark replaced `code_vmas` with `guards_ever` (same slot) and added
`caps_ever` after `id`. main's assert is still `== 80`, and mine (owners +
private_rings) also reads 80. THE TWO 80s AGREE BY COINCIDENCE, not by layout,
and the merged struct carries BOTH field sets. So the rule stands: keep ONE
assert, DERIVE the size by compiling the merged struct, extend main's field
enumeration, and never resolve it by deleting the assert.

### 6 (UPDATED). Its stated reason is gone; re-read before relying on it.
The rule "every VMA relink goes through vma_insert_in / vma_remove_in" was
justified by B-2b's code_vmas counting, which capmark removed (vma.c -11 lines).
Going through the helpers is still the tree's pattern. At merge, re-read
main's vma.c for any NEW per-relink bookkeeping rather than carrying this
obligation's old reason forward.

### 7 (UPDATED). Main is writing the guard, off main.
With code_vmas gone, nothing makes a double vma_remove_in loud, so it is now
wholly silent. Main took the linkage guard and a double-remove regression test
(head AND interior VMA) on branch `vmaguard`, off 060cbcc1f, before B-2c. Main
re-measures the call sites on main and will ring 0202 before it lands. At merge:
take main's guard, and make sure none of my vma.c hunks (vma_free,
vma_free_deferred, vma_detach_range_in) bypass it.
