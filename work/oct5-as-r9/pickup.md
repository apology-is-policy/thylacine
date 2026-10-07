# AS-R9 pickup -- corona, 2026-10-05

Checkout `/Users/northkillpd/projects/thylacine-corona`, branch `corona/async-memory`.
Base `5ff62b78809846af4780ec41f82d1676e7584e80` (= astra's tip; all six handoff
`base.json` source hashes match). **HEAD is still the base: nothing committed.**

## State: source complete, NOTHING BUILT OR RUN

Blocked only on the mac lease (main holds it for the signal7 landing gates;
corona is #1 in the queue and must re-issue `yip hold` inside every 15 min).

## What is in the working tree (18 files, `.claude/settings.json` NOT for commit)

Repair -- `kernel/burrow.c`, `kernel/include/thylacine/burrow.h`:
- `burrow_charge_claim_locked(v, as)` -- the claim with `v->lock` already held.
  `burrow_charge_claim_in` is now lock + that + unlock (one implementation).
- `burrow_unref_settled_in(v, payer, &refund)` / `burrow_unref_settled(v, Proc*, ...)`
- `burrow_release_mapping_settled_deferred(v, payer, &refund)`
Each folds the decrement, the `{0,0}` decision and the claim into ONE hold of
`v->lock`. Non-qualifying drop leaves the record alone; qualifying drop takes it.
Never touches `v` after the dropped ref is gone. Refund is a scalar so the caller
applies it outside the leaf lock. `payer == NULL` settles nothing.
Mapping form qualifies on `freed || shared_out`, `shared_out` read under the same
lock (monotonic false->true, so a later read only ADDS a reason to settle).

Callers migrated (5): `kernel/loom.c` x2, `kernel/weft.c` x2, `kernel/vma.c` x1
(via new `vma_free_settled_deferred`; `vma_free_deferred` is now its no-payer
wrapper, declared in `kernel/include/thylacine/vma.h`).

`kernel/syscall.c` -- SYS_JIT_DESTROY keeps claim/restore. PROVEN sound; the
three-part premise is written at the site. Premise 1 is fragile: a failure return
added BELOW the mutation point in `burrow_unmap_reporting` silently makes it a UAF.

Tests: `kernel/test/test_burrow.c` (4 new), registered in `kernel/test/test.c`;
`kernel/test/test_addrspace.c` async-owner settle moved onto the settled drop.

Docs: `docs/ASYNC-SERVICE-STATUS.md` (park superseded + dated corona section),
`docs/AUDIT-TRIGGERS.md` (AS-R9 addendum + PROSECUTE list on the VMO/BURROW row),
`docs/JOURNAL.md`, and 4 dossiers (`sub-kernel-burrow` carries the analysis;
vma/loom/weft corrected). `quaestor lint` 0 fail, 2 pre-existing warns. Rendered.

## DONE OFF-LEASE (operator-cleared; both are reproducible, both green)

- `python3 work/oct5-as-r9/verify-verbatim.py` -- 4/4 fixture functions verbatim.
- `CC=/opt/homebrew/opt/llvm@22/bin/clang CFLAGS="-isysroot $(xcrun --show-sdk-path)" \
   python3 work/oct5-as-r9/asr9-fixture.py` -- **12/12 legs, 5/5 mutants.
  AS-R9 IS REPRODUCED.** Evidence: `asr9-fixture.json`, `leg-*.log`, `mutant-*.log`.
  The `-isysroot` is mandatory: llvm@22 defaults to a nonexistent MacOSX26.sdk.
- `sh work/oct5-as-r9/syntax-check.sh` -- all 8 edited files parse, warning counts
  equal to base (weft 1/1 and syscall 68/68 are pre-existing).

Committed, in order:
- `4c3289b1b` the repair (settled drops + 5 caller migrations + JIT premise + 4 false
  "benign window" claims deleted)
- `6015f1100` the pre-lease syntax-check record
- `3a202c608` self-audit: ONE implementation per drop decision (the unsettled forms
  became wrappers; the `{0,0}` rule was duplicated, which was my own drift hazard)
- `c84ac94a5` the bug-CLASS sweep: 318 drop sites, no instance of AS-R9's class
  survives in production code (`work/oct5-as-r9/uaf-class-sweep.py`)

## BLOCKED ON DISK, not on the lease (as of 2026-10-05 ~19:40Z)

A from-zero `--config ci` bake needs roughly **14-21G** (`usr-rs` alone is 8.7G in
`thylacine/build` and 15G in `thylacine-s7ci/build`). Free space is **9.47 GiB** --
not enough. My base also predates main's `disk_floor_check` (`grep -c
'MIN_FREE_GB\|disk_floor'`: main's `tools/build.sh` = 7, base `5ff62b788` = 0), so
there is NO floor guard here and the failure mode is ENOSPC, which broke every
agent's Bash tool this morning (OPEN-BUGS 10:44Z) and took main and aux down together.
**Do not attempt the bake until free space comfortably exceeds the need.**

Settled so far: aux got operator approval and removed `thylacine-aux-tc/build`,
`thylacine-aux-r2/build` and `thylacine-aux/build`; disk went 6.10 -> 9.47 GiB, a real
yield of 3.37 GiB against 9 GB of apparent `du` size -- aux's APFS-clone prediction
was right and my `du` sum was an overstatement. Do NOT quote `du` sums as reclaimable
space again.

The remaining answer is **main's `thylacine-s7ci/build` (21G)**, its signal7 CI-image
worktree, whose removal at end-of-legs is already named as part of the OPEN-BUGS
10:44Z remedy. Asked on yip call **0167**; main was not in a live session, so it
rings at their next session start. If main frees it: ~9.5 -> ~30 GiB and the bake is
safe. Mac lease: main held it with ~2.1h left; my queue entry is CANCELLED at astra's
request (do not re-queue while blocked on space -- it only blocks aux).

## RUN THESE, IN THIS ORDER, ONCE THE LEASE LANDS

1. (DONE above -- re-run only if burrow.c changes)
   12 legs. Expect the 6 pre-fix/control legs to hit their distinct exit codes
   and the 6 repaired legs to exit 0. **It has never run. Until it does, there is
   no reproduction -- do not call it one.** (Apple Clang 17 is fine for a plain
   build; the ASan startup hang astra reported is why llvm@22 is the default here.)
2. `python3 work/oct5-as-r9/verify-verbatim.py` (already green: 4/4 verbatim).
3. `tools/build.sh kernel --config ci`
4. `tools/test.sh` -- watch the new `burrow.settled_*` /
   `burrow.unmap_failure_leaves_mapping_attached`, plus burrow/vma/weft/loom/
   capacity/resource/addrspace.
5. Mutants: see `work/oct5-as-r9/MUTANTS.md` (owed).
6. Models: `specs/burrow.cfg` + its three buggy cfgs, `specs/capacity.cfg`.
7. `tools/ci-smp-gate.sh` -- this is an SMP race fix; a single-CPU green proves
   little. The Oct 1-2 waiver has EXPIRED.
8. Report the SHA + evidence to astra on yip call 0161; she reviews before
   integration. **Do not spawn reviewer subagents** (AGENTS.md single-agent rule;
   astra is the reviewer).

## Traps

- `work/` is NOT gitignored. Never `git add work/` or `.claude/settings.json`.
- Do NOT merge current main: it has unreconciled reader-role/lifecycle work.
  But main @1032ac49a DOES carry all five unsafe sites (shape verified), so
  coordinate before anyone lands in burrow.c/vma.c/loom.c/weft.c. Noted to
  main+aux on yip 0161.
- Astra's `work/oct5-as-r9/` and `work/oct4-async-service/.../paused-owner/` are
  HERS: read/copy only, never execute in her checkout.
- Keep 128 MiB protection; private async + clipboard stay nondefault.
- Next arc step after AS-R9 qualifies: review the preserved UNBUILT owner draft
  at `thylacine-astra/work/oct4-async-service/owner-integration/paused-owner/`
  (base c822021a2) against the repaired base. Then MM0-MM4.

## THE LEASE WINDOW IS NOW A SCRIPT (2026-10-06)

Run `work/oct5-as-r9/lease-runbook.sh` -- do not re-derive the order from prose.
It carries all six stages with their discriminating expectations, its own
free-space floor (this base predates main's disk_floor_check), the TLC jar
re-fetch (/tmp was cleared by the 10-06 reboot), the paired pool/ramfs trap, and
a denominator control on the ELF content check. Stage 0 (the specs) needs no
image and no artifacts, so it runs regardless.

THE CLONE APPROVAL IS SPENT, and the acknowledgement is DISCHARGED (corrected
2026-10-06 from yip 0161 t17). The cache-copy acknowledgement Astra conditioned
her approval on was received and accepted on 0169 t5/t6 -- do NOT send it again
-- and her build/ is no longer held stable for me. She asked for a NEW
coordination check before any re-clone, since her tree may have moved. So
CLONE_APPROVED=1 is obsolete and no longer does anything: the runbook uses MY
OWN build/ cache, and re-reading her tree requires CLONE_RECHECKED=1 set by hand
AFTER asking her. A discharged approval is not a standing one.

## MERGE-TIME OBLIGATION (recorded 2026-10-06, from main on yip 0176)

When this branch merges main @f6f4c0397 (loom-mc, "waiters fan in"), add

    loom_drive_moved_locked(l);

immediately before the `spin_unlock(&l->lock)` in `loom_post_pool_cqe`
(kernel/loom.c:778). LOOM.md 8.6 makes the drive_gen bump the rule for EVERY
CQE post path; loom_post_cqe gets it from loom-mc, and this one would be the
lone exemption.

WHY IT IS NOT DONE ALREADY, so nobody reads this as an omission: the whole
mechanism arrives WITH loom-mc. In this tree `grep -c drive_gen kernel/loom.c`
is 0 and `loom_drive_moved_locked` does not exist in loom.c or loom.h, so the
call would not compile. The merge commit is its earliest possible home.

NOT MINE, and worth knowing for the archaeology: loom_post_pool_cqe is
pre-existing at base 5ff62b788 (astra's line), absent from my diff. main
attributed it to me; corrected on 0176. Check for sibling post paths from the
same era when merging.

main's own argument that it is not a missed wake AS MERGED is sound (the ENTER
re-samples loom_cq_ready under l->lock when registering its CQ hook, and the
post wakes cq_waiters after) -- but it is a property of the current caller set,
not an invariant, which is the reason to add the unconditional bump rather than
to skip it.

### The sibling set is COMPLETE -- enumerated, not assumed (main on 0176 t3, verified here)

main gave the provenance and the full set, and I re-derived both rather than
take them:

- `loom_post_pool_cqe` arrived on Astra's line in **c822021a2** (2026-10-04,
  "Pair Loom completions with explicit payload receipts"). It is NOT on main
  (merge-base 8746a8a2), so it is an obligation for whoever integrates
  codex/astra -- me for this branch. main is putting the site and its author
  into handoff 046 so Astra sees it too.
- EVERY writer of `l->cq_tail` in this tree, measured:
    :245  loom_create_layout      -- init to 0, NOT a publish path
    :726  loom_post_cqe           -- a COMMENT, not a write
    :760  loom_post_cqe           -- real publish; takes the bump from loom-mc
    :808  loom_post_pool_cqe      -- real publish; THE ONE SITE I MUST FIX
  (main cited 762/810; I measure 760/808 -- a two-line offset from my own edits
  above them, same functions.)

So the merge obligation is exactly ONE site, and that is an enumerated claim
rather than a guess about a set nobody counted.

Also from main, for the integration order: devno-u64 (t_stat.devno widened in
place to 64 bits) lands on main next; its trial merge into corona/async-memory
conflicts in the same 26 files main already does and ADDS NONE. Rules in
handoff 046.

## STATE AT 2026-10-07 00:40Z -- read this section FIRST; it supersedes the stage prose above where they differ

HEAD `de5e1056a`, 33 commits off base `5ff62b788`. NOTHING pushed, NOTHING
landed on main. Tree clean except the deliberately-untracked green-pair
artifacts (`work/oct5-as-r9/cpu1-green-pair/{pool.img,ramfs.cpio,.config,
system.key}` -- only MANIFEST.txt is committed). No lease held.

**AS-R9 IS UNQUALIFIED FOR EXACTLY ONE REASON: `ci-smp-gate` has never run.**
Everything else on this arc is done and evidenced.

### The lease window, when the await fires

    SPECS=0 PI_AXIS=0 sh work/oct5-as-r9/lease-runbook.sh > work/oct5-as-r9/run-<stamp>.log 2>&1

Claim inside the 2-minute offer window. Do NOT pass `CLONE_APPROVED=1` (obsolete;
astra's approval is SPENT). RELEASE THE MAC the moment the gate ends, not when
the write-up ends. WRAPPER TRAP: a backgrounded `cmd > log; echo "exited $?"`
makes the harness report ECHO's status, so a task can say exit 0 for a FAILED
run -- read `runbook exited N` in the task output, never the task's own code.
Mac was main's (seam-90 close, ~2.9h from 23:40Z); I am queue head with a
durable request. The background await is cwd-pinned and refuses any reading
unless `yip presence` shows my own row: the yip CLI resolves relay state
RELATIVE TO THE CWD, and from outside the tree it reports every resource FREE
with no queue, which is the one reading that fires a waiter.

### D7: cured and attributed, entry deliberately still OPEN

Cause was the unequal external Stratum input, not the repair. The kernel ELF
`1fe1ba3a46219dc1` and `.config 4fcc788d6be38b80` are BYTE-IDENTICAL across the
red run (hashes recorded at the time in docs/ASYNC-SERVICE-STATUS.md:758-760)
and the green run (work/oct5-as-r9/cpu1-green-pair/MANIFEST.txt); only
ramfs.cpio and pool.img moved. The kernel was the control variable, held fixed,
and D7 flipped. Green evidence: boot-logs/boot-confirm-232503Z.log:3495 (probe
PASS), :3776 (20 cycles), :3914 (boot OK), 1834/1834, 0 EXTINCTION.
It stays OPEN because 1/1 is not the 2/2 bar I set for the red, and because
astra refused the inference that the 5x10 matrix exercises D7 fifty times
(0161 note 17): the close condition is MEASURED per-boot witnesses.

### What stage 5 now measures (new, commit de5e1056a)

`tools/smp-multiboot.sh` discarded every PASSING boot's log (one shared
`build/test-boot.log`, copied aside only on a non-PASS classification), so the
witnesses had nowhere to come from. It now takes `SMP_KEEP_LOGS=1` (default OFF)
and keeps each boot's serial + harness log under `build/multiboot-logs/`,
archiving a prior run of that label. Stage 5 exports it and counts, per label,
boots that REACHED the overlapping-login ladder vs boots that reported PASS,
and REFUSES when a label's retained-log count is not N. Four arms tested on
synthetic logs; exit codes measured without a pipe.

### Stage 4's oracle (the reason the gate never ran, twice)

The witness/tally/skip assertions read `guest-test.log` -- test.sh's STDOUT, 29
lines, ZERO `[test]` lines -- while the suite's 1835 `[test]` lines live only in
the boot log. It had never passed since caacdf468. Now they read the preserved
`$BOOTLOG` behind a denominator control (the oracle must carry `[test]` lines at
all, else the SEARCH is broken).

### The orphan: a P3 observability gap, NOT an I-39 hole

`joey: reaped adopted orphan pid=4536 status=1` after boot OK is the DESIGNED
5d EXITKILL outcome: kernel/proc.c:4798-4801 is the string-only wrapper
(`code = (msg=="ok") ? 0 : 1`), so `proc_group_terminate(p, "debugger exited")`
at kernel/devproc.c:989 yields exactly 1; dap-probe's `shutdown()`
(usr/dap-probe/src/main.rs:472-477) kills ambush without an explicit detach,
which devproc.c:978-989 names as that path's trigger; the debuggee is a
`main.parkLoop` that cannot exit on its own. joey's `reap_adopted_orphans`
(usr/joey/joey.c:3791-3822) is a post-BOOT_COMPLETE sweep of ALREADY-DEAD
zombies, so the reap line is the first sweep, not a lifetime. What survives is
filed: nothing can distinguish that designed terminate from a real teardown
failure, and nothing asserts on a DAP-launched child's disposition.

### Call state

Floor on 0161 is ASTRA's. Notes 15-18 sent (pair preservation, two corrections,
and the retention mechanism). She asked for no reply during the wait. Next on
that call is the gate itself, with every label's clean-boot count AND the
per-boot D7 witness counts, reported separately from D7's cure.

## IN FLIGHT RIGHT NOW -- 2026-10-07 06:15Z onward (read before touching anything)

**THE MAC IS HELD BY ME and the gate run is EXECUTING.** Lease taken 06:15Z with
a 4h TTL; aux is queued behind me at 2.5h waited, so the lease is released the
moment the GATE ends, not when the write-up ends. Run log:
`work/oct5-as-r9/run-1007T061510Z.log`, launched as

    SPECS=0 PI_AXIS=0 sh work/oct5-as-r9/lease-runbook.sh

If you are a fresh instance reading this while it still runs: DO NOT start a
build, a boot, TLC, or anything else that takes cores. The gate records per-boot
wall clock (smp-multiboot.sh) and that number answers the #200 exposure question,
so competing work does not merely slow it -- it corrupts the measurement. Read
the log, wait for the task notification, then run
`sh work/oct5-as-r9/gate-report.sh work/oct5-as-r9/run-1007T061510Z.log`, which
quotes the run's own asserted lines rather than re-deriving them, and release the
mac (`yip release mac`) before writing anything up.

### OPERATOR VOTE 2026-10-07, NEVER RE-ASK: mac gate alone, residual recorded

thyla-pi is unreachable on BOTH routes (mDNS name does not resolve; the
cloudflared tunnel answers `websocket: bad handshake`, no local cloudflared), so
the A72/KVM axis cannot run. AS-R9 qualifies on M2/HVF from the 50-boot matrix
plus the per-boot D7 witnesses, and the missing axis is an EXPLICIT owned queued
residual that astra reviews by name -- recorded in docs/ASYNC-SERVICE-STATUS.md
and owned by the thyla-pi entry in OPEN-BUGS. It does NOT license reading the mac
green as a two-axis qualification, and only the pi booting the repair closes it.

### What the gate now produces that it did not before

`SMP_KEEP_LOGS=1` keeps every boot's serial + harness log under
`build/multiboot-logs/`, so stage 5 can count per label how many boots REACHED
the overlapping-login ladder against how many reported PASS. That is astra's
close condition for D7 (note 17): measured witnesses, never an inference from
five green rows. Nothing on the retention path is suppressed, and the reader
refuses a log set that is short, stale or unreadable.

## STATE AT 2026-10-07 07:0xZ -- THE GATE PASSED, ONE AXIS

AS-R9 is QUALIFIED ON THE MAC AXIS. `ci-smp-gate: PASS`, `runbook exited 0`,
50/50 CLEAN boots (all six non-PASS categories zero on every label), D7 ladder
reached 50/50 with probe PASS 50 and FAILED 0, suite 1834/1834 with the four
burrow witnesses RAN+PASSED, `[skip] lines: 0`. Verdict tree e673db5b9, recorded
in the post-ci-smp-gate provenance block. Run log
work/oct5-as-r9/run-1007T061510Z.log; report via gate-report.sh (exit 0 = every
section present). Mac RELEASED at gate end after 44m; aux is queue head.

Reported to astra on yip 0161 note 27, D7 separate from the AS-R9 verdict.

NOT CLOSED BY THIS: the thyla-pi A72/KVM axis never ran, so this is a ONE-AXIS
qualification of an SMP race fix (operator vote: mac gate alone, residual
recorded). The OPEN-BUGS thyla-pi entry owns it.

AMBUSH PIN (aux, 0179): answered. My build path now pins
AMBUSHFORK=$HOME/projects/ambush-pin-073faaa with a loud refusal, because aux is
moving shared master to c3c7914 which deletes held_on_thylacine.go that my
pre-cf296caa1 build.sh:118 still demands. Tested four ways. The TOOLCHAIN half
(`go list -tags thylacine_held`) is unverified by choice and gets confirmed at the
next bake -- if it fails there it is mine to fix, not a reason to reclaim cores.

UPDATE 08:3xZ -- HER REVIEW LANDED AND ITS THREE ITEMS ARE RETURNED (0161 t23/t24).
Scoped source review of cd7711eee: NO new blocking correctness defect in the
settled-drop core, the five caller migrations or the JIT exception. Not blanket
approval of the branch and NOT activation clearance. Three handoff items, all
closed without a lease: R1 the integration manifest (work/oct5-as-r9/
INTEGRATION-MANIFEST.md + make-manifest.sh, excluding 55cfdb54c without
reverting it); R2 the spec evidence (work/oct5-as-r9/spec-evidence-1006T1112Z.log
-- the full TLC output was NEVER retained because my own spec stage captured it
into a variable and printed only greps, the same verdict-without-capture defect I
fixed in smp-multiboot a day earlier; the stage now retains per-cfg output and
refuses on an empty file); R3 the unmap prose, narrowed with the two uncovered
refusals named (burrow.c:1240 null-Proc, :1249 overflow) and the premise
re-grounded on an enumeration.

ALSO DELIVERED TO MAIN (0181): two build.sh patches they approved and will land
inside the tag-pool run -- the STRATUM_SRC ledger line and the prot-mirror
wiring. They hold copies with sha256s; both OPEN-BUGS entries stay mine until
they confirm. aux consented to the prot-mirror failure mode (0179, now closed);
their fork move no longer waits on me.

NEXT: astra closes review (hers), main confirms the land (hours away). Then, post-review, wire tools/check-prot-mirror.py
--expect-unmirrored 5 into build.sh at the next bake, and the STRATUM_SRC ledger
gap (ledger/manifest recording only, astra t13). Do NOT open the paused
private-owner draft before her review.

---

## 2026-10-07 09:5xZ: AS-R9 QUALIFIED, review CLOSED, private-owner port LANDED ON BRANCH

HEAD `f05aefbff` on `corona/async-memory`, 57 commits off base. Tree clean apart
from the untracked `cpu1-green-pair/` artifacts. **Nothing pushed, nothing landed.**

The lines above about "HEAD is still the base" and "nothing committed" are the
October 5 record and are long superseded; read them as history.

### Done and NOT to be redone

- AS-R9 qualified on the mac axis 2026-10-07 06:58Z: `ci-smp-gate` PASS, 50/50
  clean boots, D7 50/50, suite 1834/1834. Run log `run-1007T061510Z.log`.
- Astra's scoped review CLOSED at `ba0c8f60f`, no blocking defect.
- Main holds both build.sh patches on `tagpool` (`6be56e59a`, `a61898766`) and
  re-measured `--expect-unmirrored 5` on their tree. They land with tag-pool and
  main will say so on 0181. ONLY THEN close the two OPEN-BUGS entries. Do NOT
  re-deliver the patches.
- The three unmap refusal assertions are WRITTEN (`df2856ca8`) and UNRUN. Two of
  the three turned out to be undiscriminatable through that API and are labelled
  BEHAVIOUR pins; the third, `burrow.unmap_interior_start_refused`, is the only
  load-bearing one and is covered with the geometry that makes it so.
- The private-owner port is authored (`213b695f8`) with the retirement in the
  settled form Astra prescribed, plus its off-lease evidence (`f05aefbff`).
  Verified: `kernel/loom.c` has ZERO claim/restore sequences and three settled
  sites. Do not re-derive the finding; `private-owner-reconciliation.md` holds it.
- The retirement double's 5-row matrix passes, including the row that matters --
  an unconditional refund PASSES the all-final legs and FAILS the nonfinal one.
  `private-owner-logs/private-retire-matrix.log`.

### NEXT, in order, when the mac lease lands

A background watcher holds the FIFO place by re-issuing `yip hold` every ~8 min
with a 5s wait (NEVER a long blocking hold -- the yip server is serial). It exits
when the mac is mine and the harness re-invokes.

1. `tools/build.sh kernel --config ci`, then the suite. Expect 1836 (1834 + the
   two new tests). The private-owner port has NEVER COMPILED -- expect to fix
   build errors before anything else is meaningful.
2. Both RED legs: `sh work/oct5-as-r9/private-owner-red-legs.sh`. Do NOT redo
   them by hand -- the script is written, and its mutation anchors, the mutants'
   compilability and the byte-exactness of its reverts are already verified
   off-lease. It mutates, builds, reads the per-test verdict out of the BOOT LOG
   (ABSENT reported as its own outcome), reverts, and runs its green control
   LAST after its own rebuild, because a sabotage run leaves its kernel in
   `build/` and `test.sh` boots THAT. Expect 4 rows: both fixtures FAIL under
   their own mutant, both PASS on the rebuild.
3. `tools/ci-smp-gate.sh` -- the retirement is an I-32 settlement path, so it
   needs the multi-boot matrix for the same reason AS-R9 did.
4. The five owning dossiers' pass (`sub-kernel-loom`, `-handle`,
   `-boot-sequence`, `-death`, `-jobctl`), deferred on purpose until the guest
   confirms the arithmetic. `No-dossier-change` trailers on the two port commits
   carry the reason.
5. Regenerate `INTEGRATION-MANIFEST.md` (`sh work/oct5-as-r9/make-manifest.sh`).

### Open and mine

- The pi A72/KVM residual: re-measured 2026-10-07, unreachable by BOTH routes
  (`thyla-pi.local` does not resolve; the cloudflared tunnel gives
  `websocket: bad handshake`). Operator vote stands, NEVER re-ask: "mac gate
  alone, residual recorded". Only the pi booting this repair closes it.
- `loom_post_pool_cqe` never consults `service_closing` -- latent, not live
  (ZERO callers, measured), enqueued against the engine chunk.
- Merge obligations, both carried forward: main `f6f4c0397` loom-mc needs
  `loom_drive_moved_locked(l)` before the `spin_unlock` in `loom_post_pool_cqe`;
  main's tag-pool `loom.c:2210` pump budget is reconciliation only (verified: no
  settlement path, no `v->lock`).

### Constraints, unchanged

128 MiB protection retained; private async, replacement memory accounting and
clipboard non-default; NO reviewer subagents (Astra reviews); never touch a
peer's lease, jobs, checkout or artifacts; Astra's four protected drafts
untouched and her tree unmodified (the port was authored here, not copied); do
NOT merge main; `.claude/settings.json` (`55cfdb54c`) is EXCLUDED from delivery,
not reverted.

---

## 2026-10-07 10:4xZ: THE PORT IS BUILT, BOOTED AND BOTH LEGS ARE CREDITED

HEAD `475eeb27e` on `corona/async-memory`. Tree clean apart from the untracked
`cpu1-green-pair/` artifacts and the red-legs `pristine/` byte copies (both
deliberate). **Nothing pushed, nothing landed.** Mac lease taken 09:46Z, 3.0h;
main is queue head behind me and I release the moment the matrix ends.

### Done today, DO NOT REDO

1. **Build**: first whole-kernel compile of the port -- ZERO errors.
2. **Suite**: 1836/1836 PASS, 0 FAIL, 0 skip, 0 EXTINCTION, banner present.
   Both new witnesses green; the three `burrow.settled_*` ones still green.
   1836 is DERIVED from `kernel/test/test.c`'s registration table, and the
   suite's own tally equalled it -- two routes, one number.
3. **Both RED legs credited** (`private-owner-logs/red-legs/20261007T103115Z`):
   each fixture reddened on its own mutant, for the assertion that mutant
   targets, as the ONLY failure, with the kernel's own 1835/1836 tally agreeing
   both times. Leg 2 mutates the ACTUAL `loom_private_destroy`, which is
   astra's PO-R2. Green control 1836/1836 on a kernel byte-identical to the
   pre-legs one (`5ced18c43ae8302a`).
4. **Astra's note 34 (six wrapper defects) all fixed**, plus a 77-check,
   16-scenario harness (`red-legs-wrapper-test.sh`) that tests the RUNNER, not
   its parsers. It discriminates: 77/77 against the fixed runner vs 11 WRONG
   against the pre-fix one (logs
   `private-owner-logs/wrapper-test-po-r5-{fixed,prefix}-20261007T114942Z.log`).
   CORRECTION, and keep it: an earlier version of this line said "57-check,
   11-scenario ... 57/57 vs 26 WRONG". The 57 run was real but its log was never
   retained, so the retained `wrapper-test-new.log` read 53 and astra caught the
   mismatch (0161 t33). Quote retained logs, not runs. That 53 log contains S11
   but not S12 (astra, t35), so "before S11 and S12" was also wrong.
5. **The parser was rewritten** after the first real run refused: a verdict is a
   STATE in the log, not a line. `test.c` prints the name before running the
   test, `sched_dump_runnable()` lands between name and verdict, 87 of 1836
   PASSING verdicts are split the same way, and the log is CRLF. Validated
   against two REAL logs. **A red leg DOES carry a tally** -- the old belief
   that it does not was never measured and is wrong.
6. **Both build.sh OPEN-BUGS entries CLOSED**: main landed 6be56e59a +
   a61898766 on main under cb7194c10, verified here with
   `git merge-base --is-ancestor`. Do NOT re-deliver.

### IN FLIGHT as this was written

`work/oct5-as-r9/smp-matrix-on-qualified-image.sh`, launched 10:40Z, log
`private-owner-logs/smp-matrix-1007T1040Z.log`. 5 rows x N=10 = 50 boots,
`SMP_KEEP_LOGS=1`. Read its own `MATRIX PASS/FAIL` line and the per-row
`ROW-RESULT` lines; the evidence dir is printed at the end. If it did not
finish, the row results file is the truth, not the task's exit code.

### THE ONE BLOCKER, and it is not the port

`tools/ci-smp-gate.sh:140` opens with an unconditional `tools/build.sh kernel`,
which REFUSES in this tree: `build/pouch/stratumd-cmake/CMakeCache.txt` and
`build/host-stratum/CMakeCache.txt` were generated from a PEER's source tree
(D7's residue via the APFS clone of their `build/`). So the gate SCRIPT cannot
run here; the matrix runner above runs its matrix STAGE on the qualified image
instead, with the rows DERIVED from the gate rather than retyped. **Never report
that as `ci-smp-gate.sh` passing.** Enqueued in OPEN-BUGS, owned, not worked
around. Main's tree is clean on this; the asymmetry is ours alone.

TWO CLAIMS OF MINE AROUND IT ARE WITHDRAWN and must not be repeated: that a
canonical rebuild would bake "54 peer-uncommitted files" into the image (all 54
are .md/.tla, none a build input) and that canonical "lacks the session-DEK lease
work" (`install-dek` is in 3 files in BOTH trees; the grep shows no difference).
D7's cure WAS a peer stratumd -- measured -- but the mechanism is not.

### NEXT, in order

1. Read the matrix verdict. Release the mac (`yip release mac`) the moment it
   ends -- main is queue head and has been waiting since 09:46Z.
2. The five owning dossiers' pass, now UNBLOCKED (the guest has confirmed the
   arithmetic). `quaestor owner` says: update `sub-kernel-loom`, `sub-kernel-vma`,
   `sub-kernel-weft`, `sub-kernel-burrow`, plus the owners of handle.c, main.c,
   proc.c, syscall.c. **`sub-kernel-loom.md` line 27 still says the private
   service lifecycle is "approved but not implemented", which is false on this
   branch -- that is the first edit.** `kernel/test/*.c` are UNOWNED and quaestor
   asks for a NEW dossier; 111 of 137 test files are likewise unclaimed, so that
   is a pre-existing gap to weigh, not a thing to invent in a hurry.
3. Regenerate `INTEGRATION-MANIFEST.md` (`sh work/oct5-as-r9/make-manifest.sh`).
4. Report to astra on 0161 with the actual numbers -- she is owed the native and
   SMP results after the wrapper correction, and asked for no acknowledgement
   beyond that.
5. The pi A72/KVM residual stays open. Operator vote stands, NEVER re-ask.

---

## 2026-10-07 12:1xZ: ASTRA'S REVIEW IS CLOSED; PO-R5 TOOK TWO ROUNDS

HEAD `54e206d0e`, 77 commits off base `5ff62b788`. (SUPERSEDED LATER THE SAME
DAY: the branch was PUSHED to both mirrors by operator decision -- branch only,
never main, no force. Nothing landed.) Nothing pushed, nothing
landed, no activation, and **no lease was taken for any of this** -- it is stub
coverage, prose and a journal entry. Tree clean apart from the deliberate
untracked evidence. No guest, no background task.

### Astra's close (0161 t33, t35)

PO-R2, R4, R6 CLOSED for the observed evidence; she independently re-read all 50
retained serial logs and both red legs. Matrix-stage-only qualification accepted
AS STATED, explicitly not as `ci-smp-gate.sh` passing. No activation or landing
clearance. External ARC/Clade fixtures and the pi remain unrun.

### PO-R5, the reusable-wrapper repair -- TWO ROUNDS, and the second was hers

Round 1 (`5674f059c`): `reap_owned` waited on the immediate build child only, so
a build's compilers -- which outlive the shell that launched them -- could still
be writing while source was restored. Ancestry cannot find them (a grandchild is
reparented to init the moment its parent exits), so every owned command now runs
as its OWN PROCESS GROUP and the group is reaped and then PROVEN empty by pgid.
A surviving QEMU used to print WARNING; unproven quiescence now refuses the
restore. And the unqualified marker was cleared on a green suite even when the
final source/HEAD checks had failed -- it now takes both halves.

Round 2 (`54e206d0e`), found by astra in round 1's own fix: `group_members`
PIPED ps INTO awk, so a ps that died with no output read as an empty group --
the gauge that reads zero because it never started, in the function whose job is
to prove a negative. Snapshots now keep their status separately and are
CONTROLLED: a process table that does not contain this shell did not observe
this machine. Unknown propagates like a live member. S17 (ps fails), S18 (ps
answers without looking), S19 (positive control: a working ps must PROVE empty).

Harness is 19 scenarios / 92 checks. Three-way at ONE harness version: 92/0 on
the fix, 84/8 on round 1 (its runs restored source with a dead ps), 73/19 on
pre-PO-R5 `ff936ff9a`. The exit status discriminates NOTHING on S15/S17/S18 --
all three runners refuse, the older two for an unrelated reason.

### Two figures of mine were wrong, both corrected in place

1. "57 checks, 57/57 vs 26 WRONG" -- the run was real, its log was never
   retained, and the retained log read 53. Quote retained logs, not runs.
   (The 53 log contains S11; only S12 was missing, which was a second error in
   the same sentence.)
2. "the grep defect affects EVERY agent in this project" -- measured FALSE.
   astra's Codex env resolves to /usr/bin/grep; main and aux each CHECKED their
   own load-bearing counts and are clean. The exposure is per-INVOCATION.

### Enqueued and owned

- The agent's `grep` (Claude Code's embedded ugrep 7.8.4) silently undercounts a
  quantified negated class: 757 of 1836, exit 0, no stderr; `-P` is correct.
  Reproduced in six lines after an earlier session failed to and rightly
  declined to record a hypothesis. OPEN-BUGS + memory lesson.
- `make-manifest.sh` was DELETING merge obligations (3) and (4) on every
  regeneration, including main's verbatim loom.h contract text -- by the command
  the manifest's own header tells the reader to run. They now live in
  `work/oct5-as-r9/MERGE-OBLIGATIONS.md`, included verbatim, and the generator
  refuses when it is absent.
- The stratumd CMake cache provenance (unchanged, still the biggest one).

### NEXT, nothing urgent and nothing lease-bound

1. Astra may reply on 0161 t36. If she reviews the ps-snapshot control, expect
   source review again, not a guest run.
2. The pi A72/KVM residual stays OPEN; operator vote stands, NEVER re-ask.
3. `kernel/test/*.c` are still UNOWNED by any dossier (111 of 137 test files),
   which belongs with the standing dossier backlog, not a hurried invention.

---

## 2026-10-07 13:4xZ: THE FLOOR CLEARS, PRESERVATION IS A SCRIPT STEP, MAIN MOVED

HEAD `d98839268`, 88 commits off base `5ff62b788`, both mirrors verified at tip
by `ls-remote` per URL. Tree clean apart from the deliberate untracked evidence.
Nothing of mine is running: no background task, no monitor, no QEMU naming this
tree. NOTHING LANDED -- measured, not asserted: 0 of 88 commits in range are
reachable from main.

### THE FLOOR PASSES FOR THE FIRST TIME TODAY, and it is not a promise

`df -m` reads 9445 MiB (9.22 GiB), so `df -g` truncates to 9 and the runbook's
`FLOOR_GB=8` PASSES. The cause is astra's OPERATOR-AUTHORIZED cleanup of her own
inactive Cargo caches and 14 post-run scratch pools (0161 t45): ~1.65 GiB net,
and du again overstated it. SHE QUALIFIED IT EXPLICITLY and the qualification is
binding: this clears the threshold AT AN INSTANT, it is not reserved capacity.
So the plan is unchanged -- claim, RE-MEASURE, release immediately if short
rather than burn the next waiter's turn. Do not lower the floor. Do not re-plan
on anyone's du totals.

### MAIN MOVED cb7194c10 -> 25ed27f21 (18 commits). DO NOT MERGE IT

exit-close LANDED, and the loomwb WIP commits are on main too. Consequence for
the delivery: ALL FOUR merge obligations now target code ON main -- f6f4c0397
loom-mc (1), cb7194c10 tag pool (2), d8b177156 + ef64e4b3a loomwb (3),
25ed27f21 exit close (4) -- each tested with `merge-base --is-ancestor`, not read
off a log. Recorded in `MERGE-OBLIGATIONS.md`. NOTHING IS APPLIED. Obligation
(4)'s anchor report was measured against the exit-close BRANCH, so re-measure its
anchors and field offsets against 25ed27f21 at merge rather than inherit them.

### THE LEASE PLAN, UNCHANGED IN SHAPE, CHANGED IN THE SCRIPT

    SPECS=0 PI_AXIS=0 sh work/oct5-as-r9/lease-runbook.sh > work/oct5-as-r9/run-<stamp>.log 2>&1

aux holds the mac (took it 14:19Z, ~2.8h, hunting a NONDETERMINISTIC lantern
failure); I am queue 1 with the durable request. TAKE NO CORES WHILE THEY HOLD IT
-- they are hunting a race and their measurement is timing-sensitive; I told them
so on 0186. Claim inside the 2-minute offer window.

Two changes in the runbook since the last section, both tested off-lease:
- `EXPECT_TESTS` is DERIVED from kernel/test/test.c's registration table, not
  pinned (it was 1834 and went stale when the port added its two tests). Derives
  1836 today. It names `/usr/bin/grep` BY ABSOLUTE PATH because the agent's
  embedded ugrep undercounts that exact pattern on that exact file (757 of 1836,
  exit 0, no stderr -- re-measured today, still reproducing), and it REFUSES
  rather than lower the bar if the count collapses.
- `preserve_boot_inputs` clones the four boot inputs out of `build/` AFTER the
  post-build Stratum pin is verified and BEFORE stage 3. The placement is
  load-bearing in both directions and moved once already: earlier, a run about to
  be rejected for wrong provenance would take a generation slot and two rejected
  runs would evict both qualified sets; later, a RED run would preserve nothing,
  and a failing run's inputs are what diagnosis needs. Bounded to 2 generations
  because a clone's shared blocks become REAL when the original is rebaked
  (~283 MiB per pool), so an unbounded history would feed the disk blocker.
  Tested by `work/oct5-as-r9/preserve-inputs-test.sh`, which EXTRACTS the
  function from the live runbook: 22 checks / 5 scenarios, and discriminating --
  flat-copy sabotage 6 WRONG, removed-refusal sabotage 2 WRONG.

### PRESERVATION STATE -- read the manifest, it is the honest version

`work/oct5-as-r9/private-owner-qualified-kernel/` now holds both kernel flavours
(.elf and .bin), the PRE-BOOT pool and its key twin, and .config.
- `thylacine-undefined.bin` is a RECONSTRUCTION, labelled as one, and astra
  credits it as reconstructed (t45). QEMU boots the flat binary, never the ELF,
  and `build/kernel-undefined` was gone before any copy existed. Regenerated from
  the retained ELF (`kernel/CMakeLists.txt:343`, bare `objcopy -O binary`) to
  `5193ee5f914f96ae` -- EQUAL to the hash the matrix log recorded before the
  loss -- with the default pair as a positive control.
- The pre-boot pool is `pool.img.baked-snapshot` `9384c245b6f1cb5b`, NOT the live
  `pool.img`: `smp-multiboot.sh:315` restores the snapshot before every boot. My
  matrix header had hashed the live file, naming an artifact no boot ever read.
- `ramfs.cpio` `63d781afe4a6e5f5` is LOST, hash-only. NOT reconstructible
  off-lease. CONSEQUENCE: the set pins what the kernel and pool were but CANNOT
  be booted as a set, so reproducing the 10-07 matrix is a NEW measurement.
  astra asks this stay explicitly disclosed. Do not substitute a re-bake for it.

### OPEN CALLS and who holds each floor

- 0161 astra -- floor MINE. She has NOT reviewed the preserve fixture and claims
  no full-gate completion. Her standing calls: keep cpu1-green-pair (~771 MiB,
  hers, do NOT delete), keep the floor, prune nothing of a peer's.
- 0186 aux -- floor mine, nothing owed. They will report `df` when they release.
- 0187 main -- floor THEIRS, and a real decision is pending: `pool_restore`
  returns 0 SILENTLY when the snapshot is missing, so an N-boot row can stop
  being N independent boots with no signal. Shared tools/ file, their gates ride
  on it. I offered (a) announce only, (b) announce + refuse for the gate,
  (c) refuse unconditionally; I lean (b) and am NOT patching it unilaterally.
  Enqueued in OPEN-BUGS as P2 so it does not rest on the call.

### STILL OPEN, unchanged

The pi A72/KVM residual (operator vote stands, NEVER re-ask: mac gate alone,
residual recorded). The 111 unclaimed `kernel/test` dossier files. The five
owning dossiers' pass needs `quaestor`, which is a Go build and therefore cores
-- deferred until aux releases, deliberately.

### TIMESTAMP CORRECTION (and the mechanism, so it does not recur)

The stamps in this section and in three other files were first written an hour
fast -- 14:1xZ / 14:3xZ / 14:4xZ for events at 13:1xZ / 13:3xZ / 13:4xZ. CAUSE,
which is the useful part: I read mtimes out of `ls`, which prints LOCAL time
(then UTC+1), and wrote them with a `Z`. Corrected here, in
MERGE-OBLIGATIONS.md, in the preservation MANIFEST.txt and in the OPEN-BUGS
entry. `date -u` and `TZ=UTC stat -f %Sm` are the only two sources to quote.
AND THE OFFSET IS NOT A CONSTANT, which is why "UTC+1 today" was the wrong thing
to write down even though it was true at the time: this host's /etc/localtime was
relinked to Europe/Prague mid-session, measured off the commits themselves --
every commit up to 14:03:15Z carries +0100 and the 14:57:52Z one carries +0200.
A stamp transcribed either side of that boundary is off by a different amount, so
there is no offset to remember. Read UTC directly or do not quote a time.
ONE KNOWN RESIDUE, deliberately not chased: the armed thyla-wake watcher's
--say text says "re-verified 14:4xZ". Re-arming to fix a cosmetic stamp was not
worth any risk to a FIFO place; the preconditions it refers to were verified at
13:4xZ and the instruction it carries (re-measure the floor) stands.

### STANDING RULE, ADDED 13:5xZ: CHECK `yip resources` AFTER EVERY COMPACTION

Before anything else, post-compaction. The reason is a hazard aux surfaced on
0189 t3 and is now fixing: `tools/thyla-wake.sh` and `tools/thyla-selfcompact.sh`
BOTH type into this pane, and if a wake and a `/compact` land in the same
fraction of a second their keystrokes can interleave -- a line beginning
`/compact` swallows the rest as compaction instructions, eating the wake.

WHY IT MATTERS HERE and not just as a lost notification: the blocking `yip hold`
still SUCCEEDS, so a swallowed wake means I am HOLDING THE MAC WITHOUT KNOWING,
while main sits queued behind me (they armed at 13:45Z). A silent lease blocks a
peer for up to the TTL. One `yip resources` turns that from "silently blocks
main" into "noticed within one tool call".

If it shows `mac HELD by corona`: the lease is already running, so go straight
to the floor re-measure and the runbook, and do NOT re-arm or re-request.
Also OWED to aux on 0189: whether a wake delivered across a compaction at all.
Their "survives your own compaction" is REASONED from the pane checks, not
observed -- they compacted with no watcher armed. Report either outcome, and
the negative is the more useful one.

### STAGE 4's DERIVED GUARD IS VERIFIED AGAINST REAL LOGS -- do not redo it

The EXPECT_TESTS derivation was first tested only standalone (it yields 1836).
The STAGE around it has now been driven as EXTRACTED from the live runbook
against real boot logs, three arms, exit statuses measured WITHOUT A PIPE:

    build/multiboot-logs/default-smp4-2.log   tests: 1836/1836  -> exit 0  PASSES
    work/oct5-as-r9/run-1007T061510Z.log      tests: 1834/1834  -> exit 1  REFUSES
                                              ("total 1834 != expected 1836")
    a log with no tally at all                                  -> exit 1  REFUSES
                                              ("NO SUITE TALLY AT ALL")

The 1834 arm is the discriminating one: it is the real AS-R9 checkpoint log, and
the derived expectation correctly refuses it because that image genuinely had two
fewer tests. So the guard still bites after being made derived.

THE WRAPPER TRAP RECURRED, in a new dress, and it nearly made me report the
opposite. I first ran each arm as `( ... ) 2>&1 | sed 's/^/    /'` and read `$?`
-- which is SED's status, so BOTH arms printed "exit 0" and the refusal looked
like a pass. The pickup already warned about the `cmd > log; echo "exited $?"`
form; the general rule is the one to carry: ANY wrapper between the command and
`$?` -- a pipe, an echo, a backgrounded job -- replaces the status you meant to
read. Measure the status with nothing after it, or capture to a file first.

## 2026-10-07 after the second self-compaction: the preserve step is fixed, the dossiers are current

STATE: HEAD dc9e10fef, 98 off base 5ff62b788, both mirrors verified at tip per
URL. NOTHING LANDED (0 of 98 reachable from main). No lease held, nothing of mine
running, no watcher of mine armed. main holds the mac with aux queued behind
them; none of my remaining work needs cores.

WHAT CLOSED HERE, do not redo:
- THE TWO PRESERVE-STEP DEFECTS, 01f9854e5. A run now writes TWO generations
  (postbuild/postgate); the bound counts generations and refuses anything below
  one run's own; .config comes from build/.config and is named for the flavour
  READ OUT OF THE FILE. preserve-inputs-test.sh is 44/0/0, and its new S8 arm
  drives the premise from the real build/ tree -- against the pre-fix runbook it
  reddens and names `build/kernel/.config` by itself. Two further mutants redden
  only their own arm.
- ASTRA'S t47 PRESERVATION REQUEST. run-20261007T140647Z-postgate holds 9 of 9
  inputs; all five hashes she named independently agree, including
  thylacine-undefined.elf 9a9252b1e92140cb which I had not measured before she
  quoted it. Labelled RETROACTIVE in its PROVENANCE.txt.
- THE DOSSIER PASS, dc9e10fef. sub-kernel-proc carries the latch's position and
  guarantee; death, jobctl and caps are dated current with their own bounded
  co-tenancy check, following chg-2026-08-15-stale-by-cotenancy rather than
  skipping them. quaestor: 0 fail, no view churn, kernel/proc.c no longer stale.
- OWED TO AUX, now sent as 0193: a detached watcher DOES survive a compaction and
  a line typed after it lands (observed -- the nudge watcher's line was the first
  turn of this context). The window DURING the compaction is still unmeasured and
  must not be reported as answered; thyla-wake types immediately where the nudge
  watcher polls capture-pane first.

OPEN CALLS: 0161 floor is ASTRA's (my t48 asks her to decide syscall 127).
0186 has my bye pending with aux. 0192 CLOSED. 0193 is with aux, who is offline.

TWO THINGS THE NEXT SESSION MUST NOT GET WRONG:
1. SYSCALL 127 IS ASTRA'S, not mine -- 417c8caeb, in my base because my base IS
   her HEAD; my delta to syscall.h is EMPTY. main has TAKEN 127 for B-2b
   (b2 608efb1dd). Do not renumber anything here: it rebuilds the kernel and
   voids the qualified artifact this checkpoint's verdict names. Merge
   obligation (6) holds it.
2. THE LOCAL TIMEZONE CHANGED MID-DAY (+0100 -> +0200, /etc/localtime relinked
   ~14:32Z). Every `ls`/`stat` reading needs TZ=UTC; there is no offset to carry,
   and the correction I wrote this morning named one as if there were.

NEXT, none of it needing a lease: astra's reply on 127 and on the preservation;
then the standing backlog (111 unclaimed kernel/test dossier files, and MEMORY.md
at 17931 bytes against a 17000 target). No new arc without operator direction --
the private-owner draft stays shut, and there is still no Main landing or
activation clearance.

## 2026-10-07 ~16:1xZ: the reap leg is written and rehearsed; the lease is queued

STATE: HEAD 3d6644272, 104 off base, both mirrors at tip, tree clean of tracked
modifications. NOTHING LANDED. No lease held.

ONE BACKGROUND PROCESS OF MINE, and it must be accounted for at every
checkpoint: thyla-wake pid 29516, armed as corona on pane %2, blocking on
`yip hold mac`. It TAKES the lease when it becomes mine and types a wake line
into the pane. Queue at arming: aux HOLDING (landing gate, ~4h left), main
queued, corona position 2. AFTER ANY COMPACTION, CHECK `yip resources` FIRST --
if the wake was swallowed by the compaction window I would be holding a machine
two peers are queued for, silently.

WHAT IS READY TO RUN THE MOMENT THE LEASE LANDS:
  sh work/oct5-as-r9/reap-leg-run.sh
Stage 0 refuses on: not holding the lease (driven RED against the real yip),
tracked modifications, the leg missing from the fixture, a broken derivation
(expects 1836, DERIVED), and the disk floor. Stage 1 is the control, stage 2 the
mutant, stage 3 restores and rebuilds so build/ is left holding a clean kernel.

PRECONDITION THAT IS NOT SATISFIED YET: free disk was 7 GiB at the rehearsal,
under the script's 8 GiB floor, because aux's gate is baking. The script refuses
until the volume recovers. DO NOT LOWER FLOOR_GB -- that is precisely how a
shared volume reaches a peer's own 6 GiB floor and breaks their build mid-landing.

THE LEG'S SCOPE, stated so the next session does not overclaim it:
  - it proves the ring's AddrSpace lifetime reference is TAKEN, via the mutant,
    whose expected outcome is the NAMED extinction "AddrSpace final lifetime
    drop with private rings" (kernel/addrspace.c:127), fired in the DYING PROC
    at proc_free -- not a UAF in the retirer (astra corrected my first
    prediction on 0161 t53 and she is right).
  - it does NOT prove the reference is RELEASED. There is no observable for
    that: kernel/addrspace.c has only a monotonic id, no live/destroyed counter,
    and the suite's leak tests (test_phys_leak_10k, test_slub_leak_10k) measure
    their own loops, not a suite-wide ledger that would notice one leaked
    AddrSpace. Adding a counter is a PRODUCTION EDIT, which astra's t53
    explicitly does not assume -- so the limit is recorded, not closed.
  - the fixture TU compiles (single-file -fsyntax-only, exit 0, no diagnostics);
    the leg has never executed.

ASTRA'S STANDING DIRECTIONS from t49/t51/t53: lifecycle axis first; keep the leg
narrow; do not replace the pinned settlement checks; do not read the dead
AddrSpace; label the fixture UNRUN until the control/mutant run under my lease;
bring a discovered lifetime defect with its evidence BEFORE writing a fix; and
the 109-file kernel/test ownership backlog is NOT a task for this handoff -- only
the operator can assign it.
