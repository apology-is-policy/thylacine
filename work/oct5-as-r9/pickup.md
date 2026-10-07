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
