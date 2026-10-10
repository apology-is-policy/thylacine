# AS-R9 pickup -- corona, 2026-10-05

**Current Astra pickup:** `work/oct5-as-r9/astra-takeover/pickup-latest.md`.
The older sections below are preserved historical evidence; use the latest
pickup/flight and canonical source before acting on their historical steps.

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

## 2026-10-07 ~16:4xZ: astra's two pre-lease findings, both real, both fixed

HEAD 5acae9b33, pushed to both mirrors and verified per URL. Tree clean of
tracked modifications. Still NOTHING landed, merged or activated; the leg is
still UNRUN; aux holds the mac (~3.8h left at writing), main is queue 1, I am
queue 2, disk 8.0 GiB.

SUPERSEDES THE SECTION ABOVE on one point. That section says the release half has
"no observable". The sequence is more useful than that: there IS a page-granular
observable (the final lifetime drop runs proc_pgtable_destroy, so an unreleased
reference strands page tables, which phys_free_pages sees after a drain) and it
was written, then REMOVED, because its INSTRUMENT cannot be made sound in this
fixture. Do not re-add it without reading this.

F1, from yip 0161 t55 -- THE RELEASE GAUGE IS OUT, not normalised.
  - Her premise, confirmed at the source: phys_free_pages returns
    g_zone0.total_free_pages alone (mm/phys.c:267) and mag_free stashes an
    order-0 page with flags cleared, "magazine ownership, not free list"
    (mm/magazines.c:134). My baseline was taken WITHOUT a drain and compared
    after one, so earlier legs' magazine residue could fabricate the delta or
    cancel a leaked page. The tree's symmetric discipline is test_cow.c:249-253
    (drain before AND after, claim as a delta between two runs one variable
    apart); I had copied test_slub.c:152/160, whose asymmetry survives only
    because nothing is in a magazine at its position.
  - Why normalising was not enough: magazines_drain_all (mm/magazines.c:154)
    walks EVERY CPU's magazine with no lock and no IRQ mask. The other 24 call
    sites free on the CPU they measure from, so for them that is a hazard only.
    THIS leg's retirer frees the dying image on whatever CPU it ran on, making
    the cross-CPU pass LOAD-BEARING for the reading, in a suite that runs at
    main.c:874 after smp_init at :689, -smp 4, with kthreads runnable.
  - So the leg witnesses ACQUISITION. To restore the release witness you need a
    quiescent drain or a production counter, PLUS an omitted-put-only mutant to
    show the gauge can redden at all -- the balanced mutant does not test
    release (astra's condition, recorded verbatim in the fixture and the
    dossier).
  - The drain's cross-CPU exposure is ENQUEUED as a tracked bug against
    mm/magazines.c (memory OPEN-BUGS.md) and sent to main (0187 note 1) and aux
    (0186 note 3) one-way. Reading only: nothing run, no corruption observed,
    no claim that it explains any past result.

F2 -- RECOVERY IS ALL-EXIT in work/oct5-as-r9/reap-leg-run.sh (now 301 lines).
One recover() that every path arrives through: quarantine FIRST (rename the
mutant images out of build/ -- same volume, no cores, no disk, cannot be
refused -- leaving build/ imageless, kept as evidence with a WARNING.txt);
source restore NONZERO on a hash mismatch; the clean rebuild only while the
floor holds AND the lease is still mine, both RE-MEASURED there, failing closed
on a byte-identity mismatch or a failed build; the run's own status preserved,
overridden with 9 only when recovery is incomplete; the lease line on every
path. The script still never releases the mac itself -- a finding is
investigated with the machine held.

NEW, and run it before trusting a change to the runner:
  sh work/oct5-as-r9/reap-leg-recovery-test.sh      # 24 checks, 7 scenarios
Functions are EXTRACTED from the live script so they cannot drift; the LAYOUT
arm asks the real build tree, never the stub. Driven both ways: three mutants
one variable apart redden their named arm only (23/1, 22/2, 23/1) against 24/0
unmutated. RUN=<path> drives a mutant copy.

WHAT IS READY THE MOMENT THE LEASE LANDS, unchanged in its experiment half:
  sh work/oct5-as-r9/reap-leg-run.sh

## 2026-10-07 ~17:0xZ: the run script's ORACLES are validated against real logs

Everything below is off-lease verification of things that would otherwise have
failed INSIDE the lease window, where a wasted hour costs a 24h re-queue.

THE THREE ORACLES IN reap-leg-run.sh NOW MATCH REAL LOGS, both sides. A stub only
tests my reading of a format, so each pattern was run against a retained log
rather than reasoned about:
  - '\[test\] loom\.private_owner_lifecycle \.\.\. PASS' -> 1 match in
    work/oct5-as-r9/boot-logs/boot-confirm-140947Z.log, whose real line is
    "    [test] loom.private_owner_lifecycle ... PASS" (indented; the pattern is
    unanchored, so the indent does not matter).
  - '  tests: [0-9]+/[0-9]+' -> 1 match, "  tests: 1836/1836 PASS", and 1836 is
    exactly the number the script DERIVES from test.c's registrations.
  - '^EXTINCTION:' -> validated against a REAL extinction log
    (private-owner-logs/red-legs/20261007T103115Z/interior-unmap-serial.log):
    anchored count 1 and UNANCHORED count 1 AGREE, so the line truly starts at
    column 0 and nothing is hidden behind a prefix. Corroborated by the tree's
    own ABI consumer: tools/test-fault.sh:177 greps the identical '^EXTINCTION:'.
    This one mattered most -- an anchor that did not match would have made BOTH
    the control's extinction check and stage 2's "no OTHER extinction fired"
    check FAIL OPEN, i.e. pass while blind.

THE PRESERVE-STEP EXTRACTION IS FAITHFUL. Stage 1 sources the step out of
lease-runbook.sh with an awk range; a stale anchor would hand it an empty file
and leave preserve_boot_inputs undefined mid-run. Measured now: 125 lines, sh -n
clean, exactly one function definition, all four REFUSING guards, the PINNED
mechanism, all NINE source pairs, ending at the function's own closing brace,
and preserve_boot_inputs IS defined when sourced.

THE MANIFEST IS REGENERATED and its figures are current: tip 010656e03, 110
commits in range, both mirrors at that tip, main still 25ed27f21b5b, 0 of 110
reachable from main, and exactly ONE commit (55cfdb54c) touches .claude/ so the
EXCLUDED claim still holds. The regeneration left MERGE-OBLIGATIONS.md
BYTE-IDENTICAL and all six obligations appear inside the generated file -- the
protection added after a regeneration destroyed obligations (3) and (4) is
proven rather than assumed.

A MIRROR EPISODE WORTH KNOWING ABOUT: GitHub rejected the 010656e03 push four
times over ~7 minutes with a server-side 500 (Internal Server Error) while
ls-remote against it worked, its status page read all-operational, the pack was
25 objects with an 8.5 KiB largest blob, --no-thin reproduced it, and codeberg
accepted the identical push immediately. SSH is not provisioned here, so HTTPS
was the only route. A background retry reconciled it at 17:01:56Z with NOTHING
changed on my side, which is what proves it was the remote and not the content.
The discipline that caught it: verify per URL with ls-remote, never read "pushed
to both mirrors" off one push's output.

## 2026-10-07 ~18:1xZ: the drain finding CORRECTED, then measured

READ THIS BEFORE TOUCHING THE DRAIN ITEM. My earlier entry called
magazines_drain_all's cross-CPU walk an UNDOCUMENTED hazard. It is documented:
vault/system/kernel/memory/sub-kernel-mm-phys.md:113 ("quiescent-only ... walks
peer CPUs' sets with no coordination; test-harness and shutdown use only") and
its Prosecution section at :336 ("any path that touches g_percpu without [the
mask] (or from a peer CPU, as magazines_drain_all does) must prove quiescence").
Source corroboration: my_cpu()'s #807 note and mag_alloc's "#807 regression
guard". Do not re-report the mechanism as a finding.

WHAT WAS OPEN AND IS NOW MEASURED (operator-directed, off-lease, commit
ce5b13334, write-up work/oct5-as-r9/mag-double/FINDING.md): the #807 guard does
NOT cover this class. Under TSan with the eight REAL bodies extracted from
mm/magazines.c and each asserted verbatim, the faithful leg produced 7575 silent
double allocations with the guard firing ZERO times (count never leaves range).
Conflicting production lines: magazines_drain_all mm/magazines.c:159-160 vs
mag_alloc:113 and mag_free:144. Attribution control (lock both sides) = 0/0.
FIX CANDIDATE VALIDATED: per-CPU self-drain = 0 races, 0 corruption.
Reproduce: sh work/oct5-as-r9/mag-double/run.sh

TRAPS THAT RUN CAUGHT, worth inheriting:
  - A LEG LABELLED "CONTROL" CAME BACK WORST (2177 double-allocs where I
    predicted 0) because it locked the owner but not the drainer, and
    magazines_drain_all takes no lock at all. A control must be checked for
    being a control; structurally, a control cannot be worse than the faithful
    configuration.
  - A SINGLE LEG'S ZERO IS A FALSE NEGATIVE: the tight-loop leg said 0 where
    another schedule on identical code said 7575.
  - A bare grep for the conflicting statement hands you mag_drain's copy: the
    same text appears in three bodies. Disambiguate by enclosing function.

STILL OPEN, needs the GUEST: whether the suite's 24 call sites satisfy the
quiescence their sanctioned use requires. Not reachable by reading.

DISK, corrected by main: swap (grown to 3072M) shares this APFS container, so
free space falls with NO builder running -- the 8.2 -> 6.7 GiB drop was swap, an
hour before main's build started. A figure in a lease line carries the time it
was MEASURED, not posted. My FLOOR_GB=8 can be crossed by swap alone, so a
refusal of mine is not evidence that a peer is consuming the volume.

WAKE WATCHER HAZARD (aux, 0193, cause CONFIRMED by capture-pane): thyla-wake
reads Claude Code's dim "keep going" suggestion as typed text, so it can acquire
the mac, fail to deliver the wake line, give up and EXIT WITH THE LEASE HELD --
it cost main and me 2.6h today. My watcher 29516 is aux's tree's copy and has
the bug. Mitigation running: a background poll that exits the moment the mac
reads HELD-by-you, bounding a silent hold to ~2 minutes. aux will announce a
fixed commit to re-arm from; do not re-arm from the old copy after that.

## 2026-10-07 ~18:4xZ: the run's off-lease preconditions are PRE-CLEARED, and the wait is bounded

Resumed from the self-compaction at 18:35Z. HEAD unmoved at 4b1394619 when this
was written; tree clean of tracked modifications. NOTHING LANDED, NOTHING RUN.

FLEET STATE, measured at 18:39Z (not recalled):
  mac   HELD by main, 10m in, 1.8h of TTL left -- B-2 land gates on b2
        4382e69b9: ci-smp-gate N=10 (5 rows), then test-fault + burrow/capacity/
        cow specs. Their runner pid 58101; their QEMU was 72535, then 80612 as
        the rows roll. NONE OF IT IS MINE and none of it is a kill list.
  queue 1: corona (me), with a durable `yip request` beside the watcher.
  disk  8777 MiB = 8.57 GiB free. `df -g` truncates to 8, and the runner's
        check is `[ 8 -ge 8 ]`, so the floor PASSES right now. main's gates draw
        ~1.7 GiB/h, so it may not still pass when the lease lands -- and if it
        does not, the refusal is correct and FLOOR_GB=8 does not move.
  watcher thyla-wake pid 63896, alive 7m, armed from
        ~/projects/thylacine-aux (HEAD 62875f9df), whose thyla-wake.sh carries
        THYLA_WAKE_HELD_BOUND -- i.e. the fix that RELEASES a lease it cannot
        announce. aux's 0196 names the fix as 251fa1ed3; the tree I armed from
        reads 62875f9df and the bound is present in the file, which is the
        property that matters. Kill by PID, NEVER `thyla-wake.sh cancel` (it
        stops every agent's watcher -- aux's second defect, enqueued by them).

THE SIX OFF-LEASE PRECONDITIONS OF reap-leg-run.sh, DRY-RUN AND CLEARED. The
point is not that they pass; it is that a HARNESS refusal must not be discovered
inside the scarce window, after a ~10 min bake has already been spent:
  P1 fixture names `unpinned-reap` 3 times (needs >= 3) and still carries
     'retires exactly once'. Worth checking because the fixture WAS edited this
     session (the release witness came out) and that edit could have taken the
     delta assertion's wording with it. It did not.
  P2 EXPECT_TESTS derives 1836 from kernel/test/test.c (needs >= 1000).
  P3 AGREEMENT WITH GROUND TRUTH, which is the half that makes P2 worth
     anything: work/oct5-as-r9/boot-logs/boot-confirm-140947Z.log reports
     `tests: 1836/1836 PASS` and carries `[test] loom.private_owner_lifecycle
     ... PASS`. Three older logs read 1834/1834, so the derivation TRACKS
     registrations rather than being a constant that happens to match.
  P4 the preserve function extracts from lease-runbook.sh at 125 lines and is
     DEFINED when sourced.
  P5 both mutation anchors are unique in kernel/addrspace.c (1 and 1).
  P6 the named extinction is still at kernel/addrspace.c:127.
A zsh no-match glob killed P3's first form (`*/*/*boot*.log` with no matches
aborts the loop) -- my own recorded trap, re-met; redone with `find`.

THE WAIT IS A BACKGROUND JOB, NOT A TURN END: a poll that exits the moment the
mac reads HELD-by-you, and -- because silence must not be able to look like
"still waiting" -- ALSO exits and reports if the watcher pid dies without the
lease, or at a 90 min deadline. Every terminal state emits a line, and the
lease-is-mine line carries the disk figure so the floor question is answered in
the same breath.

CALLS CLOSED HERE: 0187 (pool_restore, (b) landed at 3a6f8ce2b; main had already
said bye) and 0186 (the disk FYI -- the volume came back up to 8.5 GiB, so the
thread had run its course; bye is a proposal and any say by aux reopens it).
astra's note 46 at 18:38Z reviewed 4b1394619 and found the t59 claim limits and
the missing-summary rejection present, with no further correction: she is
waiting on the ACQUISITION WITNESS and nothing else from me.

### THE RELEASE HALF, ANSWERED STATICALLY (18:5xZ, off-lease, NO defect found)

The leg witnesses ACQUISITION only, and the release question was recorded OPEN.
While main held the mac I asked it of the SOURCE instead of an instrument. The
result is NEGATIVE -- the pairing is sound -- and the label matters more than
the conclusion, so it is first: THIS IS A STATIC PAIRING ARGUMENT, NOT A
WITNESS. It does not replace what I removed and it is not offered as doing so.

THE SET, ENUMERATED (kernel/ only; the lone build/kernel-undefined hit is a
GENERATED symbol table, not a caller -- `work/` and `build/` excluded):
  addrspace_private_begin   1 production site    kernel/loom.c:334
  addrspace_private_end     3 production sites   kernel/loom.c:345, :351, :431
  writes to l->service_as   EXACTLY 1            kernel/loom.c:354
  loom_free                 static, ONE site     kernel/loom.c:749

WHY THE PAIRING IS TOTAL:
  - loom_create_private has exactly three exits after a successful begin:
    !charged -> _end (:345); layout alloc failed -> uncharge + _end (:351);
    success -> service_as set (:354), after which NO further early return
    exists (the rest is assignments plus the void burrow_charge_record).
  - service_as is written once and never cleared, so loom_is_private (:323),
    the sole routing discriminator, cannot go stale. 9 production readers, all
    reading the same single write.
  - the only last-ref path is loom_unref (:744): private -> enqueue, else ->
    loom_free. loom_free is STATIC with exactly that one call site, guarded by
    !loom_is_private, so a private ring cannot reach the kfree that would skip
    the _end. (Every other `loom_free` hit in the tree is a COMMENT -- that is
    why the grep looked alarming and is not: count the call sites, not the
    mentions.)
  - loom_retirer_main (:436) pops and calls loom_private_destroy
    UNCONDITIONALLY per item; destroy releases the guard LAST (:431), after the
    uncharge, with the retired counter bumped after that.

WHAT IT DOES NOT ESTABLISH -- carry these, they are the real residue:
  1. STATIC. The code contains a release on every path; nothing here shows one
     EXECUTED.
  2. A ring whose refcount never reaches 0 never enqueues: a ref leak leaks the
     guard with this pairing fully intact. Refcount balance is unbounded here.
  3. A retirer that never runs or never drains leaks the guard SILENTLY -- the
     list has no bound and no timeout. lp_wait rests on that liveness premise.
  4. Nothing about the PAGE-accounting release: the
     addrspace_uncharge_pages(as, metadata + refund) arithmetic at :428 is a
     separate claim with a separate witness.
  5. The two extinction() arms in loom_private_destroy precede the _end, so a
     private ring in legacy state dies LOUDLY. Those arms are not leak paths.

STATUS CHANGE, deliberately narrow: "release unverified" -> "release
structurally paired over an enumerated set, execution unwitnessed." astra's
framing stands unchanged; sent as note 47 on 0161, one-way.

AND THE FIXTURE WAS NOT EDITED to record this. A comment-only change cannot
alter behaviour, but it would perturb the file under measurement and invalidate
the P1 precondition greps cleared minutes earlier. It waits until the control
and the mutant have both run. Perturbing the subject to document it is how a
measurement loses its provenance.

## 2026-10-07 19:0xZ: THE LEG RAN. Control GREEN, discrimination NOT established.

Lease held 19:00-19:09Z (8 min of host). main stopped a superseded 40-minute
ci-smp-gate to hand it over early; I released the moment the recovery hash
printed and they re-took at 19:09Z.

WHAT HOLDS -- the leg is no longer UNRUN:
  control kernel 9fae5eb3a0873190, stratum pinned stratum-astra @61dde37 CLEAN
  tests: 1836/1836 PASS   (derived expectation 1836, matched)
  [test] loom.private_owner_lifecycle ... PASS   (by name)
  test.sh exit 0, no '^EXTINCTION:' anywhere in the control boot log
  preserved 9 of 9 boot inputs, no ABSENT line -- the FIXED preserve step's
  first exercise inside a real run, which PRESERVE-STEP-DEFECTS.md had left as
  the one thing its 44-check harness could not prove.
So reachability is demonstrated: a private ring on a reaped Proc retires on the
ring's OWN image reference, through the real retirer, with no fixture pin.

WHAT DOES NOT HOLD -- and my runner reported it as success:
The mutant produced the predicted NAMED extinction, "AddrSpace final lifetime
drop with private rings", and the runner printed DISCRIMINATED. IT DID NOT
DISCRIMINATE. The extinction fired inside `addrspace.proc_alloc_in_shares`
(mutant-boot.log:511, last announcement at :510); `loom.private_owner_lifecycle`
sits at line 601 of the CONTROL log, ~90 suite lines later. The boot died before
the leg ran.
  CAUSE IN THE TREE: test_addrspace.c's private_ring_sharing_failure() -- the
  first statement of that test -- ends `addrspace_private_begin(as);
  addrspace_unref(as);` and asserts the space survives ownerless BECAUSE the
  guard pins it. Remove the guard's reference and that unref is the final
  lifetime drop with private_rings == 1. So a balanced mutation of
  addrspace_private_begin/_end can NEVER reach this leg.
  CAUSE IN MY ORACLE: the third condition was "the leg did not report PASS",
  which a leg that NEVER RAN satisfies just as well -- a vacuous negative,
  exactly [[bug-215-negative-assert-satisfied-by-broken-fixture]] and the
  gauge-reading-zero pin. Two of three conditions were real; the third was free.

BOTH FIXED, neither run:
  - the mutation is now CONFINED to loom's USE: `addrspace_unpin(as)` inserted
    immediately after `addrspace_private_begin(as)` in loom_create_private
    (kernel/loom.c), leaving addrspace.c untouched so every addrspace test
    behaves normally and the boot reaches the leg. addrspace_unpin IS
    addrspace_lifetime_put (addrspace.c:164), so this is exactly "the ring keeps
    no net lifetime reference". PRISTINE/restore retargeted to kernel/loom.c.
  - the oracle now asks POSITIVELY which test the boot was inside when it died,
    from the log's LAST ANNOUNCEMENT, and refuses when that is not this leg. It
    also refuses if the leg completed with a verdict while the extinction fired
    elsewhere. DRIVEN AGAINST THE REAL FAILED LOG: it rejects
    reap-leg-20261007T190621Z, which is the run it exists to have caught.
    The announcement pattern needed tightening too -- the suite prints summary
    lines beginning "[test] " (e.g. "[test] yield-waits: 724 invoked, ...") and
    a bare prefix match picked one up as "the last test".

A GENUINE BY-PRODUCT, worth more than the failed mutant: the property the mutant
was built to witness is ALREADY witnessed in-tree, and POSITIVELY -- that
`ownerless` assertion in private_ring_sharing_failure asserts the guard's
reference keeps an ownerless space alive, with no mutation needed. So the leg's
unique content is narrower than I had it: not "the guard takes a reference" but
"the RETIRER reaches the dying image through the ring's own reference, end to
end, with no fixture pin" -- which is what the control green now shows.

ALSO LANDED THIS WINDOW: the runner was building against the WRONG STRATUM. It
called tools/build.sh with STRATUM_SRC unset, so build.sh fell back to
~/projects/stratum/v2 while build/'s stratumd cache was configured from the
pinned stratum-astra at 14:09:56Z. CMake's source-mismatch error is the ONLY
reason it failed loudly instead of building a control against a different
Stratum than the leg was qualified against. Fixed at b5943591d by EXTRACTING the
runbook's pin block (not copying it), with a denominator control that refuses if
STRATUM_PIN_FULL is absent, since rev-parse alone succeeds in any tree holding
the object and the HEAD-equality is the discriminating half.

STILL OPEN: the acquisition witness. It needs one more lease window with the
confined mutant. Everything else for it is in place and pre-cleared.

### CORRECTION, ~19:3xZ: the confined mutant I committed was UNBALANCED

Caught off-lease, by doing the per-caller enumeration I had just written down as
the lesson from the failed run. It would have cost the second window.

THE DEFECT IN MY FIX: `addrspace_unpin(as)` after `addrspace_private_begin(as)`
cancels the ring's GET correctly (net 0 ref, +1 ring at create), but
loom_private_destroy still calls `addrspace_private_end`, whose PUT then has no
matching get. Every ring create/destroy cycle therefore nets **-1** on the
AddrSpace lifetime refcount. The early legs (:73, :126, :144) each retire a ring
while their Proc is still ALIVE, so the count would reach zero with an owner
still holding the space and `addrspace_lifetime_put` would fire its FIRST guard
-- "AddrSpace final lifetime drop with live owners" (addrspace.c:120) -- or
"addrspace_unref of an already-released AddrSpace" (:118). Both are the WRONG
extinction in the WRONG leg. The hardened oracle would have refused it correctly,
which is some comfort, but the window would be gone.
The balanced addrspace.c mutant did not have this problem because it removed the
get AND the put together; confining it to loom re-opened the balance question
and I did not re-ask it.

THE BALANCED CONFINED FORM, now installed, two sites both in loom.c:
  loom_create_private  `addrspace_unpin(as);`        cancels the GET
  loom_private_destroy `--as->private_rings;` under as->lock, REPLACING
                       `addrspace_private_end(as);` -- removes the unmatched PUT
Net per cycle: 0 ref, 0 rings, and the ring holds NO lifetime reference, which is
the condition under test. `private_rings` is a plain u32 (addrspace.h:163) and
loom.c already touches `as->` fields, so this needs no header change. Both
anchors asserted unique (the BARE `addrspace_private_end(as);` appears 3x in
loom.c and is NOT usable as an anchor), and the runner REFUSES if only one half
lands -- a half-applied balanced mutation is precisely the unbalanced one.
Syntax-checked with both halves applied: errors=0; loom.c reverted
byte-identical (cbdd71f6f5ee4c74).

WHY TEST-GRANULARITY ATTRIBUTION SUFFICES HERE, verified per leg rather than
assumed -- which matters because the fixture packs EIGHT create sites into ONE
test, so "died inside this test" does not by itself name the leg:
  :120 :138 :159  lp_wait the ring to retirement BEFORE dropping the Proc, so
                  private_rings == 0 at the drop -- cannot fire.
  :171 :172       takes its own addrspace_pin BEFORE dropping, so the drop is
                  not final -- the masking this leg exists to remove.
  :248 :249       drops the Proc with the ring OUTSTANDING and NO pin held.
                  THE FIRST LETHAL POINT IN THE TEST.
  :312            the cleanup drop, after this leg, so it cannot precede it.
TWO PATHS LEFT DELIBERATELY UNBALANCED, and named rather than fixed:
loom_create_private's two failure exits (:345 !charged, :351 layout-alloc) still
call `_end`, so they would over-put by one under the mutant. Neither is
exercised: the fixture's only refusal (:66) returns from loom_measure BEFORE
addrspace_private_begin, so the guard is never taken on it. Mutating three sites
to cover an unreachable path adds more risk than it removes.

#### The mutant's outcome is DETERMINISTIC -- verified, not assumed (19:4xZ)

The obvious worry about a mutant whose ring holds no lifetime reference is a
RACE: if the retirer destroyed the ring before the dying Proc reached its
lifetime drop, private_rings would already be 0, no guard would fire, and the leg
would just PASS -- a burnt window with an unreadable result.

IT CANNOT HAPPEN, and the reason is an ordering in proc_free that I had asserted
in the fixture's own comment and had not checked: `addrspace_unref(p->as)` at
kernel/proc.c:699 runs BEFORE `handle_table_free(p->handles)` at :720, and :686
says so on purpose ("P3-Da: release the address space here, BEFORE
handle_table_free"). So at :699 the handle table is intact, the Loom still holds
its refcount, loom_unref has not run, nothing is enqueued, and private_rings is
1. addrspace_unref drops the owner, drains the VMAs, then puts the lifetime
reference -- so the guard's checks are reached in exactly the state the
prediction names (owners 0, vmas drained, private_rings 1) and fire the third
one. The retirer never gets a turn: the enqueue would only happen at :720.
That ordering is also what makes this leg's claim true in the first place -- it is
why the ring's own reference is the ONLY thing keeping the descriptor addressable
across that window.

### astra t61: three corrections applied (19:4xZ) -- and the first killed my argument

1. ARRIVAL, NOT POSITION. My per-leg "first lethal point" argument assumed
   execution follows line order. It does not: `LP_CHECK` is `goto done`
   (fixture:25) and the cleanup at `done:` does `if (l) loom_unref(l);` then
   `if (p) test_proc_drop(p);` -- so under the mutant ANY earlier check failure
   reaches an owner drop with a ring outstanding and raises the SAME named
   extinction in the SAME test, while hiding the check that failed. astra saw it;
   my enumeration did not.
   FIX (she sanctioned the fixture change, no round-trip): the fixture prints
   `[lp-mark] unpinned-reap-owner-drop` immediately before the target drop and
   `[lp-mark] cleanup-owner-drop` before the cleanup's -- the latter also
   printing the failing check's message, so the hidden failure reaches the log
   instead of being replaced by its consequence. Oracle: stage 2 REQUIRES the
   arrival marker and REFUSES on the cleanup marker; stage 1 requires the
   arrival marker too, because a control whose leg never reached that drop would
   pass without exercising what the mutant tests.
   DRIVEN THREE WAYS against synthetic logs built from the real one:
     M1 arrival, no cleanup, died in leg -> ACCEPT (rc 0)
     M2 no arrival marker                -> "THE LEG NEVER REACHED ITS OWNER DROP"
     M3 arrival AND cleanup              -> "THE CLEANUP PATH RAN", check named
   The earlier four-way set for the test-granularity arm still passes (L1 accept,
   L2 other test, L3 leg completed, L4 no announcements).
2. THE GET-CANCEL MOVED to the successful create return, after charge AND layout
   succeed, instead of immediately after the begin. This confines the net-zero
   reference change to Looms that actually exist and leaves both rollback exits
   balanced -- removing my assumption that allocation failures cannot occur
   during a mutant run, which is not an assumption a mutant should rest on.
3. THE CONTROL SET IS PINNED, out of the pruner's reach rather than by raising a
   bound: moved to work/oct5-as-r9/reap-leg-control-PINNED-20261007T190621Z/
   (a rename, no disk cost) with a PINNED.txt stating what each half of that run
   is and is not evidence of. preserve_boot_inputs prunes $PRESERVE_DIR
   newest-first to KEEP_INPUT_GENS (lease-runbook.sh:226-239), so the set would
   have survived one more run and not two. Verified 9/9 against its own
   HASHES.txt after the move; thylacine.bin = 9fae5eb3a0873190 as recorded.
   (My first verification said 0 of 9 OK: HASHES.txt stores 16-char prefixes and
   I compared them to full digests. Nine of nine failing is the tell -- suspect
   the check.)

A FRESH CONTROL IS NOW OWED, because the fixture changed. astra: "preserving the
original receipt does not require freezing the test forever."
DISK AT 19:37Z (the stamp this line first carried, 19:5xZ, was ESTIMATED and
about 20 minutes ahead of the commit that wrote it -- third time today, and the
figure beside it disagreed with the 6857 MiB I measured two minutes later):
df -g 7 against FLOOR_GB=8, so the next run REFUSES at stage 0 until it
recovers. That is the floor working, not a problem to solve by lowering it.
MEASURED SINCE, with `date -u` and `df -m` in the same command:
  19:43Z  7064 MiB  (df -g 6)
  20:06Z  8466 MiB  (df -g 8)  <- at the floor, so disk no longer refuses
The volume came back UP while main's gates ran, which is the swap behaviour aux
and I measured earlier: it moves with no builder of mine running.


== 2026-10-07 20:0xZ: THE MARKERS BROKE THE ORACLE THEY WERE ADDED TO SERVE ==
astra reviewed 97287f351 (her t63) and named two defects, both in the
INTERACTION between the arrival markers and test.c's own output, and both mine.

1. `done:` is ALSO the normal fallthrough from the leg's last check, so a
   PASSING control prints the cleanup marker -- and stage 1 refused on ANY
   cleanup marker. The oracle refused every healthy run, including the green one
   I had already measured. The fixture now names the arrival:
   `cleanup-owner-drop normal-fallthrough` or `... after-check-failure: <msg>`.
2. `test_run_all` prints `    [test] <name> ... ` with NO newline, runs the
   test, then prints the verdict, so a marker's own newline moves the verdict
   onto a later line. `<name> ... PASS` cannot exist on one line in an
   instrumented healthy run, and stage 1 required exactly that. The same newline
   silently disabled the mutant's completion check, which looked for PASS/FAIL
   on the announcement line where it can no longer appear.

Both oracles now read the LEG'S OWN BLOCK and treat the verdict as a STATE in
it: PASS, FAIL or NONE, NONE being the lethal mutant's expected state. They are
FUNCTIONS now, which is what made the arms possible.

work/oct5-as-r9/oracle-arms.sh -- 19 arms, both oracles, the real functions
EXTRACTED from the runner with a denominator control, every log built by EDITING
TODAY'S REAL BOOT LOGS at the leg's own line (so context, CRLF endings and
emission shape are the guest's), two arms real logs unedited. Every gate in both
oracles is exercised with its own required message; exactly one arm accepts per
oracle. THREE further defects the arms found, which review had not named:
  - the refusal ORDER was unhelpful: an early check failure leaves no arrival
    marker, so an arrival-first oracle said "it never reached the drop" while
    the log held the NAME of the failing check. Diagnostic gate first now.
  - an arm I wrote to cover the verdict check DID NOT COVER IT: no extinction,
    so the mutant oracle refused at its first gate and never reached the check
    whose silent death defect 2 had just demonstrated. Arms K/L reach it.
  - a refusal I could not drive: the harness's missing-base-log refusal was
    unreachable because the script cd's to the repo root. Paths overridable now,
    exercised both ways.

Also landed: every floor reading is RETAINED ($OUT/disk.txt, printed at exit) --
aux asked what one --config ci bake costs in this tree and I could not answer,
because the figures had gone to a terminal while every other piece of evidence
landed in $OUT.

@dfca09ddc, both mirrors. Fixture TU compiles (errors=0, 2 pre-existing
missing-prototype warnings). NOTHING LANDED, NOTHING ACTIVATED, no guest run.
BLOCKER STATE: disk CLEARED (8.3 GiB); mac is main's for ~2.1h. A fresh control
is owed when the lease arrives, because the fixture changed again.

== 2026-10-07 20:4xZ: THE SUITE-LEVEL CHECKS, EXAMINED AND LEFT ALONE ==
I told astra (t64) that stage 1's suite-level checks are NOT covered by the arms
-- only the leg-level oracles are. Closing that gap honestly meant examining
them rather than building machinery, and the answer is that they need none:
every parse failure I can construct fails CLOSED.

  `grep -qE '^EXTINCTION:' "$B"`      the log is CRLF; '^EXTINCTION:' is
                                      anchored at the start, unaffected.
  tally absent                        $tally empty -> $got empty -> pass/total
                                      empty -> the equality against the DERIVED
                                      expectation fails -> refuse.
  tally present, regex DRIFTS         sed leaves the line unchanged, so $got is
                                      the whole line, which begins with two
                                      spaces -- `cut -d' ' -f1/-f2` then yield
                                      EMPTY, not a wrong number -> refuse.
  a test failed                       pass != total -> refuse.
  EXPECT_TESTS somehow empty          the equality fails -> refuse; and stage 0
                                      already requires it to derive >= 1000.

So there is no fail-open direction among them and no vacuous comparison. An arm
set here would exercise string equality, not a mechanism, which is motion. What
WOULD be worth arms is any future check whose predicate is a PATTERN rather than
an equality -- those are the ones that fail open, as `grep -qF ""` matching every
line proved today.

NOT A HOLLOW CLEAN: the enumeration above is the claim, so the next session can
check my reasoning instead of re-deriving it or trusting it.

== 2026-10-07 20:49Z: DISCRIMINATED. THE ACQUISITION WITNESS IS CLOSED ==
Run work/oct5-as-r9/reap-leg-20261007T204646Z/, exit 0, 20:46:46-20:49:43Z,
three minutes on a warm build/ (the cold control at 19:00Z took nine).

CONTROL (fresh, because the fixture changed):
  tests: 1836/1836 PASS against the DERIVED expectation 1836
  leg announced + arrival marker + normal-fallthrough marker + PASS in its block
  test.sh exit 0, no extinction, control kernel dd0c4e67c7306ae0
  (NOT 9fae5eb3a0873190 -- a different kernel is CORRECT here: the fixture's
  marker text changed between the two runs. The 19:00Z receipt stays pinned at
  reap-leg-control-PINNED-20261007T190621Z/ and is still 9/9.)

MUTANT (confined to loom.c, balanced, both halves verified present):
  kernel 92f1dbb1c7de3778, differs from the control
  test.sh exit 1, ONE extinction, by name, at line 603 of mutant-boot.log:
    601:    [test] loom.private_owner_lifecycle ... [lp-mark] unpinned-reap-owner-drop
    603:EXTINCTION: AddrSpace final lifetime drop with private rings
  ATTRIBUTED: arrival marker present, NO cleanup marker, NO verdict in the leg's
  block -> it died AT the drop under test, not elsewhere in the same test.
  AND INDEPENDENTLY: HALLS frame #4 = test_loom_private_owner_lifecycle+0xb14,
  which owes nothing to my instrumentation. Worth knowing for the next mutant:
  the crash dump may already name the function, so check the dump BEFORE
  building markers -- though a marker is what lets the ORACLE decide instead of
  a human reading a trace.

RECOVERY, same exit: 2 mutant images quarantined -> mutant-artifacts-DO-NOT-BOOT,
build/ left imageless then rebuilt BYTE-IDENTICAL to the control
(dd0c4e67c7306ae0), kernel/loom.c restored and hash-verified to cbdd71f6f5ee4c74.
Tree clean, 0 dirty tracked. Mac RELEASED after 4 minutes; main was next and
their wait resolved on it.

DISK COST OF THE WHOLE RUN, the figure aux asked for at 13:33Z and nothing had
retained: 499 MiB for two --config ci bakes + a recovery bake + two full boots.
  20:46:46  stage-0             11359 MiB
  20:47:28  post-control-build  11314 MiB
  20:49:01  post-mutant-build   10878 MiB
  20:49:05  pre-recovery-build  10877 MiB
  20:49:43  exit                10860 MiB

STILL OPEN, and not weakened by this: THE RELEASE HALF. This mutant tests
ACQUISITION. A missing release LEAKS rather than extincting, so it has no loud
witness, and the enumerated structural pairing (1 begin site, 3 end sites, 1
service_as write, loom_free static with 1 guarded call site, the retirer
destroying unconditionally) remains its whole basis. Also still open and
SEPARATE: whether the suite's 24 magazines_drain_all call sites satisfy the
quiescence their sanctioned use requires -- needs the guest.

== 2026-10-07 21:0xZ: ASTRA VERIFIED IT INDEPENDENTLY; BOUNDED CLOSURE ==
astra (yip 0161 t65) re-verified the result against the artifacts rather than
against my report: all nine saved input hashes, both current default kernel
ELF/bin byte-identical to the saved fresh control (bin dd0c4e67c7306ae0...),
the quarantined mutant bin (92f1dbb1c7de3778...), loom.c restored against
pristine (cbdd71f6f5ee4c74...), tracked tree clean, external Stratum exactly
61dde37 and clean, Mac free consistent with my release. Her words for what is
established: "The acquisition/owner-reap fixture checkpoint is now established
... The older premature mutant is not counted."

THE CLOSURE IS BOUNDED, in her terms and mine:
  - NO REPEAT of this experiment is needed WITHOUT RELEVANT CHANGE. If a future
    session is tempted to re-run it, the question to answer first is what
    changed that makes the previous receipt inapplicable -- the fixture's marker
    text changing was such a reason, which is why a fresh control was owed this
    time and why the kernel hash legitimately differs from 19:00Z.
  - RELEASE SENSITIVITY STAYS EXPLICITLY OPEN, and its STRUCTURAL PAIRING stays
    DISTINCT FROM RUNTIME EVIDENCE. The two must not be reported as one thing.
  - NOT IMPLIED by any of this: full-arc completion, new SMP or Pi
    qualification, a Main landing, public/private activation, clipboard
    activation. The 128 MiB protection stays.

PINNED, per her instruction, before any later pruning could reach it:
work/oct5-as-r9/reap-leg-control-PINNED-20261007T204646Z/ (moved OUT of
$PRESERVE_DIR; 9/9 verify, width taken from the stored prefix). The OLD control
receipt and the REJECTED-mutant evidence are retained and named in its
PINNED.txt so neither gets pruned as superseded.

== 2026-10-08 05:40Z: CHECKPOINT 1's CHARGE-REFUSAL LEG, AUTHORED AND UNRUN ==
Section 5 of private-owner-reconciliation.md was ALREADY DISCHARGED by 213b695f8
(re-verified at fea85b498: loom_private_destroy does one burrow_unref_settled_in,
zero claim/restore sequences in loom.c, refund consumed from the drop). Do not
reopen it. The gap was in CHECKPOINT 1: loom_create_private's refusal edges.
astra t67 APPROVED (a) with these terms, all met in the fixture:
  - assert the routing preconditions (fresh single owner, non-exempt, the
    geometry admitted just above, a cap derived from THAT admission's charge);
  - restore charge AND private_rings AND the lifetime ref (addrspace_ref_count)
    AND the owner count to baseline -- the ref on its own, because a split
    defect could clear the count and keep the reference;
  - attribute RED to the leg's OWN assertion, never to the suite-failed
    extinction (required to be the ONLY one) or a whole-suite failure;
  - cleanup safe on both runs: a whole leaked guard is released after the
    assertion records it, so the mutant cannot carry the leak forward.
  - the stale "release witness below ... headers" comment is GONE from the
    fixture top (its includes were removed in a26bc6ab4).
(b), the layout-allocation unwind: STRUCTURAL ONLY, runtime obligation OPEN.
  NOT closure and NOT a waiver. After (a) runs, bring astra a minimal,
  test-only, locally scoped way to force loom_create_layout to fail, with
  isolation from unrelated allocations and reset on every exit. Do not design
  a general allocator fault framework.
Coverage map: work/oct5-as-r9/checkpoint1-coverage.md (DRIVEN / STRUCTURAL /
  NOT YET REACHABLE / UNRUN, never merged). "Bounded tables" is broader than
  geometry (ASYNC-SERVICE-LIFECYCLE.md slot reservation + the 64-entry envelope).

RUN IT: sh work/oct5-as-r9/refusal-leg-run.sh   (needs the mac lease; tree clean)
  Control: 1836/1836, leg PASS in its own block, normal-fallthrough marker.
  Mutant (one site, loom.c's !charged branch): leg FAIL with EXACTLY
  "refused charge leaves no guard, reference or charge", marker naming the
  same check, exactly ONE FAIL in the boot, tally 1835/1836, the only
  extinction "kernel test suite failed". Anything else is a FINDING.
  Oracle arms 18/18 off-lease: refusal-oracle-arms-transcript.txt.
  A fresh control is owed because the fixture changed; the reap-leg pinned
  control stays as THAT leg's receipt and is not superseded.
Release sensitivity stays OPEN and distinct. 128 MiB stays. Nothing activated.

== 2026-10-08 08:41Z: THE CHARGE-REFUSAL LEG DISCRIMINATED ==
Run work/oct5-as-r9/refusal-leg-20261008T083538Z/, exit 0, 08:35:38-08:38:54Z,
source 6f2bd6226 (astra t69's two corrections in). Mac held 5 min, RELEASED at
08:39Z the moment the runner exited (aux next).
CONTROL: tests 1836/1836 PASS vs derived 1836; leg PASS in its own block with
  the normal-fallthrough marker; test.sh exit 0; kernel dd0a56361d1e7b9d.
MUTANT (one site, loom.c's !charged branch): kernel e883d57ffdb5ba46; the leg's
  block = after-check-failure marker + FAIL, both "refused charge leaves no
  guard, reference or charge"; the ONLY FAIL in the boot; tests 1835/1836 FAIL;
  the only extinction "kernel test suite failed"; test.sh exit 1. No reap
  arrival marker in the mutant, as predicted -- it fails before reaching it.
  Verified from the raw logs, not just the oracle's summary.
RECOVERY: 2 mutant images quarantined; loom.c back to cbdd71f6f5ee4c74;
  build/ rebuilt BYTE-IDENTICAL (dd0a56361d1e7b9d); tree clean. Disk 184 MiB.
PINNED: refusal-leg-control-PINNED-20261008T083538Z/ (moved OUT of
  boot-inputs/, 9/9 at prefix width). boot-inputs/ now holds only
  run-20261007T140852Z.
CLAIM, BOUNDED: the charge-refusal unwind is load-bearing and the leg sees a
  leaked guard -- one boot per side, Mac axis. NOT an SMP qualification, NOT
  checkpoint-1 closure (layout-failure runtime OPEN), NOT release sensitivity.
NEXT: bring astra the (b) seam proposal (work/oct5-as-r9/seam-b-proposal-DRAFT.md)
  for review; nothing is edited for (b) until she answers.
main took the vma_remove_in linkage guard (0202 t5, branch vmaguard) and rings
  0202 before it lands; capmark landed main = 060cbcc1f (MERGE-OBLIGATIONS 5-7
  updated).

== 2026-10-08 09:25Z: (b) THE LAYOUT-FAILURE LEG AND ITS SEAM, AUTHORED AND UNRUN ==
astra t71 APPROVED (b) and answered all three questions; no further permission
round trip. Her terms, as built:
  - SEAM: KERNEL_TESTS-gated one-shot in loom.c (arm / disarm / armed accessors,
    declared under #ifdef KERNEL_TESTS in loom.h), TAKEN as loom_create_private's
    first statement. Isolation precondition RECORDED: the fixture is the only
    caller -- rescope before any concurrent or engine caller.
  - LEG: clear before arm; an armed EARLY refusal consumes the shot and the next
    admission is not faulted (checked, not reasoned); baselines BEFORE arm; cap
    asserted from the measured admission; CHARGE and GUARD/ref/owner as SEPARATE
    assertions. Cleanup: disarm on every exit; undo a retained charge and a
    whole leaked guard after the assertion records them, only once the probe has
    retired. The (a) unexpected-success goal accounting is kept.
  - CAUGHT BEFORE THE LEASE: lp_wait(goal) inside this leg would have waited for
    p's live owner too (goal counts every ring created) and failed the control.
    The probe waits on a LOCAL snapshot + 1.
  - RED: M1 = uncharge deleted -> "layout failure returns the charge";
    M2 = private_end deleted -> "layout failure releases the guard, reference
    and owner". Each on PRISTINE source, diffs/binaries/logs/stage statuses
    kept separately. Oracle arms 21/21 incl. both cross-arms.
  - SHAPE (stage 3): KERNEL_TESTS-off compile of loom.c vs the seam's parent.
    Dry-run OFF-LEASE to validate the CHECKER only: no seam symbol, names
    identical, loom_create_private identical (127 insns), whole .text identical
    (16056 B); its two positive controls refuse (tests left on -> 4 seam symbols;
    a nop in the baseline -> code DIFFERS). The dry run also caught a path bug
    ($ROOT/$SH assumed SH relative) before it cost a lease. The measurement of
    record is the runner's, under the lease.
RUN IT: sh work/oct5-as-r9/layout-leg-run.sh (lease; clean tree). One window:
  control, M1, M2, shape, recovery; lease + disk re-checked before every bake.
Still separate and open: loom_create_layout's OWN inner failure path; release
sensitivity; SMP; activation. 128 MiB stays.

== 2026-10-08 11:57Z: (b) THE LAYOUT-FAILURE LEG DISCRIMINATED, BOTH HALVES ==
Run work/oct5-as-r9/layout-leg-20261008T115213Z/, exit 0, 11:52:13-11:56:19Z,
source 9bb7e6037 (astra t73's refinements in). Mac held 6 min, RELEASED 11:56Z.
  control 1836/1836, leg PASS, test.sh 0, kernel 2b56a62ae6cc92ae
  M1 (uncharge deleted)    -> FAIL "layout failure returns the charge", only FAIL, 1835/1836, rc 1, kernel 5a8a5ac93fb18e32
  M2 (private_end deleted) -> FAIL "layout failure releases the guard, reference and owner", only FAIL, 1835/1836, rc 1, kernel 1458457252f31eaa
  shape (KERNEL_TESTS off vs parent 4783d6f92): no seam symbol, names identical,
    loom_create_private identical: 127 insns AND 13 relocations; .text identical (unrelocated)
  relocation-only CONTROL (off-guest, scratch): one CALL26 retargeted in the object
    -> unrelocated identical, -r DIFFERS -> the check refuses it.
  recovery: loom.c == pristine == HEAD; build/ byte-identical (2b56a62ae6cc92ae). Disk 421 MiB.
  PINNED: layout-leg-control-PINNED-20261008T115213Z/ (9/9). boot-inputs/ = run-20261007T140852Z only.
All verified from raw logs. BOUNDED: Mac axis, one boot per side.
STILL OPEN: loom_create_layout's own inner failure path; release sensitivity; SMP; activation.
MERGE item 8 added (vmaguard landed main = 943dcca51: disjoint hunks, placement check owed).

== 2026-10-08 12:07Z: (b) ACCEPTED; NEXT IS THE INNER UNWIND, PROPOSAL SENT ==
astra t75 (12:03Z) VERIFIED note 54 independently at a9db74956 (raw logs, both
  saved diffs, loom.c == pristine == HEAD, ELF/bin == pinned control, 9/9 input
  hashes, 127 insns + 13 relocations). The two caller-side layout-unwind
  witnesses are CLOSED at single-boot Mac scope. Preserve them; no repeat without
  a relevant change.
She NAMED the next item, so do not ask again: loom_create_layout's INNER unwind
  (loom.c:255, `if (!r) { kfree(l); return NULL; }`). Her terms: the two
  allocation failures handled distinctly; the private caller's charge/guard
  balance asserted through the real inner return; an oracle that detects a
  missing kfree of THAT Loom (g_loom_created is insufficient); no global
  magazine drain; no generic fault framework; public callers protected from
  the injected fault (the fixture-only argument does NOT carry over to the
  shared loom_create_layout); bring the seam/observation boundary FIRST if
  new instrumentation is needed.
SENT t76 (12:07Z), the boundary. NOTHING EDITED; wait for her answer:
  FAULT: a thread-keyed one-shot in loom.c. take(l) runs after the kmalloc
    succeeds, is consumed by a CAS from current_thread() to NULL, and has an
    EXPLICIT NULL-current refusal, because current_thread() is NULL before
    thread_init (thread.h:660) and a naive CAS would fault an unarmed caller.
    On a match it records the victim and arms the slub free watch on it.
  OBSERVATION: a single-slot free watch in slub's kmem_cache_free, KERNEL_TESTS
    only. It is deterministic both ways: armed while l is live, so a freed l
    is seen at the free itself, and a leaked l cannot be freed by anyone.
    mm/slub.c sits on the Allocator audit row; this is flagged to astra.
  WHY NEW: a freelist walk races with LIFO reuse (fabricated RED in the
    control); the kmalloc caches are static (slub.c:44) and shared by a size
    class; the slub totals are global.
  LEG: an instrument self-check first (watch a, free b -> not fired, free a ->
    fired); baselines before arm; separate assertions for the refusal, the
    consumed shot, the ORACLE "the inner ring failure frees the unpublished
    Loom", the charge, the guard/ref/owner, and g_loom_created (sanity only).
  RED: M1 deletes kfree(l); M2 deletes the watch hook (the self-check FAILs).
    The caller-side unwind is NOT re-mutated (astra: no repeat of the outer
    witness). SHAPE: loom.c and slub.c compiled with KERNEL_TESTS off.
  The kmalloc failure is proposed as its own STRUCTURAL row, not driven
    (nothing to unwind; its caller-side NULL was already discriminated).
  Held aside: the saturation refusal (addrspace.c:89) can be driven from a
    constructed state, with no seam.

== 2026-10-08 16:32Z: t76 AMENDED by astra t77; REVISED BOUNDARY SENT AS t78 ==
astra t77 (12:12Z) caught two real errors in t76, both mine:
  1. struct Loom > 2048 (loom.c:361 _Static_assert) -> kmalloc's LARGE path;
     kfree's large branch calls free_pages directly (slub.c:432-442), so a
     kmem_cache_free watch would NEVER fire, even on the healthy control.
  2. "watch did not fire -> still allocated -> kfree(victim)" double-frees
     under the M2 instrument mutant. An unobserved free is UNKNOWN, not owned.
t78 (revised; NOTHING EDITED): the hook goes in kfree's validated LARGE branch
  only, and compares the pointer only; the self-check uses the SAME class
  (sizeof(struct Loom), !PG_SLAB asserted while live, plus a _Static_assert
  against SLUB_MAX_OBJECT_SIZE), frees each object ONCE and asserts only
  afterwards; the victim is never reclaimed and there is no victim accessor;
  M1's leak is recorded as INTENDED; the charge/guard cleanup is proven by
  the AddrSpace counters, independent of the observer; the fault slot and
  the watch are disarmed on every exit; the arming-thread lifetime is stated;
  shape compares kfree plus every function carrying the loom code, with
  relocations. The kmalloc-NULL row is STRUCTURAL (bounded statement); no
  saturation.
WAIT for astra's answer to t78 before writing any code.
ALSO: main 0202 t13 (execquiesce WIP 6ff9f4bab) -> MERGE-OBLIGATIONS item 9:
  its sole-holder tests read addrspace_ref_count; on this branch they must read
  addrspace_owner_count. proc.c:4751 (exec walk a) is NEW and MERGES CLEAN with
  the wrong count; :684 conflicts visibly and test_virtio.c:284 guards it.
  Latent today (no production pin or guard). Told main (t14 + note 5).

== 2026-10-08 16:44Z: THE INNER-UNWIND LEG, AUTHORED AND UNRUN ==
astra t79 (16:33Z) ACCEPTED t78's boundary for implementation with 4 details,
all built:
  1. self-check: each of a/b is freed exactly once and the watch disarmed
     BEFORE any LP_CHECK; PG_SLAB is read only on live, non-NULL objects.
  2. ORDER: refusal, spent shot, watched metadata, charge and guard/ref/owner
     all come BEFORE the oracle, which is LAST IN THE LEG (later fixture legs
     follow it and do not run under either mutant -- claim nothing about them).
     The runner enforces this order; a control with the charge check moved
     after the oracle REFUSES (rc 1).
  3. "fired" = ENTRY to kfree's validated large-free site, not the buddy outcome.
  4. thread-keyed non-NULL CAS; slots reset on every exit; the victim is never
     dereferenced and NEVER reclaimed.
DROPPED from t78: the g_loom_created sanity check. Public Looms created on other
  CPUs would make it a false-FAIL source under SMP, and the refusal check covers
  publication anyway. Tell astra.
RUN IT: sh work/oct5-as-r9/inner-leg-run.sh   (needs the mac lease; clean tree)
  control: 1836/1836, leg PASS, normal-fallthrough
  M1 (kernel/loom.c, kfree(l) deleted) -> ONLY FAIL "the inner ring failure frees
     the unpublished Loom"; leaks one large kmalloc, INTENDED, never reclaimed
  M2 (mm/slub.c, watch hook deleted)   -> ONLY FAIL "a watched large free fires
     the watch"; the inner leg does NOT run under M2
  shape: loom.o + slub.o KERNEL_TESTS-off vs the parent; kfree + every emitted
     constructor compared with relocations
Off-lease, verified so far: -fsyntax-only clean for loom.c, slub.c, test_loom.c
  (the 2 test-prototype warnings are pre-existing: OPEN-BUGS line 146); oracle
  arms 21/21 (inner-oracle-arms-transcript.txt, runner bfef1dce684eaec4); the
  runner's order and anchor checks pass on the tree and refuse their controls.
NEXT: commit, push, dry-run the shape stage off-lease (checker validation only),
  bring astra the commit + runner + arms BEFORE any lease. The mac is aux's, with
  main queued.

== 2026-10-08 16:48Z: COMMITTED 470d78f4b + 4fc54ab2d; REPORTED t80; WAIT FOR REVIEW ==
The shape dry run (off-lease, checker only) found a CHECKER defect: loom_create
  and loom_create_with_receipts are 3-instruction tail calls with NO relocation.
  The per-function floors were fixed (4fc54ab2d); the real tree is green and 3
  controls refuse at their own gates (inner-shape-dryrun.txt). Arms re-run on
  the final runner 21e2fa2c24239300: 21/21.
t80 sent to astra with all four t79 details, file:line, and the dropped
  g_loom_created check. Astra's term: her review BEFORE any guest/resource work.
  Then: queue for the mac (thyla-wake hold mac), re-measure the disk (7.4 GiB per `yip resources`
  just before t80, < FLOOR_GB=8 -- the runner refuses; do NOT lower it), then
  sh work/oct5-as-r9/inner-leg-run.sh.
MERGE-OBLIGATIONS 9a (e696d9372): execquiesce's landed shape mapped onto the
  owner/lifetime split; call 0202 closed by both sides.

== 2026-10-08 17:01Z: t81 STALE-NOTIFIER FINDING CLOSED (322cd68c3); REPORTED t82 ==
astra t81 found an observer race: the watch's slot and fired flag were separate
  atomics, so a reused address freed on another CPU could load the old watch,
  stall, and store fired into a LATER arming. M1 could then PASS falsely.
FIX: the notifier is keyed to the ARMING THREAD (non-NULL current, reset on
  arm/disarm). The identity check cannot change mid-call; the arming thread's
  own store is synchronous.
CONTROL: a helper kthread frees a watched same-class object and the watch must
  stay quiet (fixture :203-222), ordered pair < cross < arm (runner-enforced).
M3: the identity check deleted -> ONLY FAIL "a watched free on another thread
  leaves the watch quiet". M1/M2 retained.
Off-lease: syntax clean; arms 25/25 (runner ea8597c1ade95a2a); anchors x3 plus
  a refusing control; shape dry run re-taken, identical, 3 controls refuse.
WAIT for astra on t82. Then queue for the mac; start only with disk >= 8 GiB
  (it was 7.3 per yip resources at t82). Do not take a lease to sit below the floor.

== 2026-10-08 ASTRA TAKEOVER: operator transfers the remaining work ==
The operator transferred Corona's entire remaining implementation and runtime
qualification to Astra after the Claude agents exhausted credits. Continue this
checkout/branch, with Yip identity only from thylacine-astra. No waiting for a
Corona reply or independent-review claim. The current pickup and work queue are
work/oct5-as-r9/astra-takeover.md; that file supersedes older WAIT instructions.
T83 correction: explicit fixture-local helper completion before the void join;
on timeout disarm and stop before the inner leg, preserving transferred ownership.
The log oracle now rejects extra failure diagnostics hidden by a final expected
verdict. New runner: 26/26 constructed-log arms; original runner: exactly the
masked-helper-timeout arm fails (it wrongly accepts), retained as a RED control.
Actual-source host control authored in cross-timeout-control.py, not yet executed.
No compiler/VM or lease owned by Astra; Main's background gate still live. Keep
8 GiB floor, existing pinned generations, external clean Stratum 61dde37 and
all prior activation/integration restrictions. Use astra-yip.sh through
THYLA_WAKE_YIP for the inherited runner's resource reads.


## October 8: resource override and actual-source host regression

The operator made Main, Aux and Corona resources available to Astra until Monday,
superseding peer-lease waiting restrictions. Main's existing SMP process was still
live and was preserved; credit exhaustion does not stop already-launched jobs.
Under the explicit grant, cross-timeout-control.py compiled and ran actual
extracted fixture code: old block rc 11 reproduces lost timeout; repaired modes
0-6 all pass (completion, timeout, late completion, allocation refusal, thread
creation refusal, foreign-thread observer mutation, timer wrap). Raw generated
sources, compile logs and result.json are retained under
work/oct5-as-r9/astra-takeover/cross-timeout/. Fixture SHA256:
4e295342f0ac6187e0ca52db69581628047dd4a71679191b0cddf1c2a3823334.
The test_loom.c syntax check passed with zero errors and only the two existing
prototype warnings. Self-review: extracted production fixture logic with host
doubles proves these bounded control paths, not ARM scheduler or runtime gates.
Guest inner-unwind discrimination remains pending; disk was 7.2 GiB against the
unchanged 8 GiB floor. No peer process, lease, source or pinned evidence removed.


## October 8, 18:20 heartbeat: disk headroom

Removed only reproducible Cargo debug caches beneath this inherited checkout's
build/usr-rs (host target debug, bare-metal target debug and shared debug build
outputs). Checked no tracked files, no symlink targets, no source-like files in
the target caches, and no compiler using the checkout. No raw test logs, pinned
inputs, sources, release outputs or peer jobs were removed. Inventory and df
readings: astra-takeover/cargo-cache-cleanup.json. Available space changed from
7.122 to 8.157 GiB; apparent du size overstates reclaimed
blocks on the shared volume. Headroom above 8 GiB remains small: recheck before
every bake and recovery. Main's gate still runs (default-smp8 boot 8/10 passed
in its raw log); this is Main's source only, no Corona qualification. No new
guest work launched and no peer lease changed. Next: preserve the running gate,
recheck its completion and disk, then record Astra resource ownership under the
operator override and run the inner-unwind control/M1/M2/M3/shape runner.


## October 8, 18:50: inner control passed; disk stopped mutant qualification

Astra reclaimed the Mac under the explicit operator grant after Main's registered
runners died and its gate ended at 18:49Z. Yip event
operator-reclaim-6137f867f1554aab83f9dbb73b60643d retains the previous lease and
evidence; the administrative pre-TTL transfer ran under Yip's OS lock, without
impersonating Main. Main's separate b0bd631a0 gate failed: 49/50 boots, one
single-CPU joey nonzero exit. Both TLC commands lacked Java, not model verdicts.
Raw observations are retained in astra-takeover/main-gate-observation/.

Inherited run inner-leg-20261008T185135Z at a89f55240, clean Stratum 61dde37:
control 1836/1836 PASS, private-owner normal fallthrough, test.sh rc 0. Default
kernel ae48cf3dcc59c047, ELF d16bcf72a632f7ff. M1 built, but NOT BOOTED: post-build
free space 8096 MiB breached the unchanged 8192 MiB floor. Runner exit 3.
M2/M3 and shape UNRUN. Recovery restored both sources to pristine == HEAD and
quarantined M1's ELF/bin in mutant-artifacts-DO-NOT-BOOT. No kernel image remains
in build/kernel; clean recovery rebuild was correctly deferred below the floor.
Lease RELEASED. Do not boot the quarantined mutant or claim discrimination.

Moved the complete control generation outside automatic pruning to
inner-leg-control-PINNED-20261008T185135Z; verified all nine hashes. Its default
inputs match this control, but the also-preserved undefined pair is the older
856ecae generation and gives NO new-source UBSan claim. Full SHA256s and scoped
result: astra-takeover/inner-run-20261008T185135Z-result.json. Self-review verified
raw control tally, normal-fallthrough block, one-line M1 diff, restored source
hashes and missing build images. Next: establish enough disk headroom for the
remaining bakes and recovery, acquire Astra resource ownership, then complete
all mutants and test-off shape. Full gates, release sensitivity and engine/MM
work remain open; all activation and integration restrictions remain in force.


## October 8, 19:58: inner unwind discriminated, bounded Mac scope

Run `inner-leg-20261008T195301Z` completed with exit 0 on `2bf48f7ad`, clean
external Stratum `61dde3727921e70e2c72fbd3c9e2044a192f4a54`. Control passed
1836/1836 with normal fallthrough. Independent pristine-source mutants each
produced exactly one expected failure, 1835/1836 and test.sh rc1:
M1 missing inner kfree -> "the inner ring failure frees the unpublished Loom";
M2 disabled observer -> "a watched large free fires the watch";
M3 removed thread guard -> "a watched free on another thread leaves the watch quiet".
M2 and M3 stop before the inner fault is armed. M1 intentionally leaks one large
allocation until its disposable VM exits; the observer proves free-site ENTRY,
not buddy reclamation.

KERNEL_TESTS-off comparison: no test symbols, identical names; kfree 43
instructions/15 relocations, layout 130/10, create 3/0, receipts 3/0, private
127/13, all byte-identical disassemblies including relocations. Recovery restored
both sources to pristine == HEAD, and ELF/bin match the saved control exactly
(bin ae48cf3dcc59c047, ELF d16bcf72a632f7ff). Mac lease released.

Control inputs moved outside pruning to `inner-leg-control-PINNED-20261008T195301Z`;
all nine hashes verified. The undefined pair in that preserved set is older
856ecae evidence, not new-source sanitizer coverage. Full SHA256s, raw result
checks and shape counts: `astra-takeover/inner-run-20261008T195301Z-result.json`.
Self-review reread all four raw boot logs, asserted sole diagnostics and verdicts,
compared saved disassemblies and verified restored sources and artifacts. This
is single-agent self-review, not an independent audit. Scope is one Mac default
boot per side. Older full gates still cover only their recorded source.

Disk was unblocked by reclaiming unused reproducible native Cargo debug caches
in Main and b2c under the operator resource grant; no tracked/source files,
release outputs, raw evidence or pinned generations were removed. Inventories:
astra-takeover/main-cargo-cache-cleanup.json and b2c-cargo-cache-cleanup.json.
APFS sharing made the first apparent 2.6 GiB deletion reclaim little; removing the
other cache brought actual free space to 9.7 GiB. This run stayed above the 8 GiB
floor, 9943 -> 9468 MiB. Prior floor-refusal evidence remains intact.

NEXT: release-half sensitivity and remaining structural/engine obligations in
checkpoint1-coverage.md, then new-source full gates and approved async-first
engine work before MM0-MM4. No Main landing, private async/clipboard activation,
or removal of the 128 MiB protection is implied by this bounded result.


## October 8: release decrement witness prepared

Added an isolated first private retirement with the creator's original owner
reference retained. After the release-published retirement counter advances,
the fixture separately checks charge/guard/owner balance and exact lifetime
reference count against its one-reference baseline. No freed pointer is read,
no additional pin masks the count, and no global magazine drain is introduced.
This measures a lifetime decrement, not final descriptor/page-table reclamation.

Self-review rejected a global omitted-put mutant because the shared helper has
earlier AddrSpace-test and rollback callers. release-leg-run.sh instead expands
only loom_private_destroy's private_end call to the current helper body, omitting
only its lifetime put. Get, decrement and locking remain intact. The expected
sole failure is "private retirement releases its image lifetime reference";
the mutant deliberately leaks one reference until disposable VM teardown.
The earlier probe precedes all rollback tests so attribution is unambiguous.
Recovery retains quarantine-first behavior, source hash checking, exact control
rebuild, Stratum pin and the 8 GiB floor checked before each bake. No production
source is changed in the committed implementation.

Syntax: zero errors, the same two pre-existing test-prototype warnings. Runner
shell syntax and 19 constructed-log oracle arms pass, including a masked earlier
failure. Raw checker transcript: astra-takeover/release-arms.txt. Guest control
and mutant are still UNRUN at this preparation checkpoint. Existing inner-unwind
results remain scoped to 2bf48f7ad; the new fixture needs its own runtime result.


## October 8, 20:08: lifetime release decrement discriminated

Run release-leg-20261008T200530Z on 2a4e501c7 exited 0. Control 1836/1836,
normal fallthrough, test.sh 0. The successful-retirement omitted-put-only mutant
failed ONLY "private retirement releases its image lifetime reference":
1835/1836, test.sh 1, sole matching diagnostic and suite-failure extinction.
Its diff expands the current private_end body at loom_private_destroy only,
retaining the guard decrement and locking; the shared helper and all rollback
callers remain untouched. The leaked lifetime reference is intentionally left
until disposable VM teardown. This discriminates the decrement while a creator
owner remains. FINAL descriptor/page-table reclamation remains OPEN and separate.

Self-review verified both raw logs, exact attribution, mutant diff, restored
source == pristine == HEAD, and rebuilt ELF/bin == pinned control (bin
 aa7b16dc8fdfad3b). Moved nine verified inputs outside pruning to
release-leg-control-PINNED-20261008T200530Z. Full hashes and scope are in
astra-takeover/release-run-20261008T200530Z-result.json; its undefined pair is
older-source evidence only. Disk 9418 -> 9021 MiB, 8 GiB floor retained. Lease
released. This is single-agent self-review, no independent audit or full-gate
claim. Next: final reclamation witness/remaining checkpoint obligations and
new-source full gates, then the approved engine and MM work. No activation,
Main integration or removal of the 128 MiB protection.


## October 8, 20:10 pickup: current-source full gate prepared

The next step is the real ci-smp-gate.sh on the current fixture: default
smp1/4/8 and UBSan smp4/8, ten boots each, all logs retained. Final descriptor
and page-table reclamation stays OPEN independently of this matrix. No kernel
mechanism or fixture changed in this preparation.

The driver now has opt-in disk admission checks before/after each build and
before each row (8192 MiB for this run), plus exact boot-input capture BEFORE
the matrix. Each flavour's config/ELF/bin is saved before the next build can
overwrite the shared config; the final ramfs/pool snapshot/key pair is saved
with full hashes. Existing destinations refuse and no history is pruned.
current-source-gate.sh checks the clean source and exact clean Stratum pin,
records Astra's lease/PID, retains the real gate's status, verifies unchanged
inputs/source on exit and releases the Mac. It does not rerun older mutant
legs or pretend to close the reclamation witness.

Self-review: standard rows, N and classifier unchanged. Forced low-space,
malformed-floor and existing-capture-directory controls refused with expected
exit 3/2/3 before any build; the existing kernel hash remained unchanged.
Shell syntax passed. Vault render/lint: 1463 notes, zero failures, one warning
for 46 stale dossiers. Raw preflight: astra-takeover/gate-preflight.txt.
This is single-agent self-review. The new runtime matrix is not yet qualified;
read the unique current-gate-* directory and gate.rc/runner.rc on pickup.


## October 8, 21:00: current-source full Mac matrix qualified

Real ci-smp-gate.sh run current-gate-20261008T201816Z on
98c50e844fcaf2d62da3705fbfa0d45de383152c completed with gate.rc=0 and
runner.rc=0. All five standard rows (default smp1/4/8, UBSan smp4/8) were
10/10 clean: 50 PASS, zero corruption, external-kill, inject-miss, timing or
other classifications. Every retained serial log reports 1836/1836, the private
owner fixture's normal-fallthrough PASS, D7 overlap PASS, twenty-cycle PASS and
the boot banner, with no extinction or kernel test failure. All twenty distinct
d7user00..d7user19 E2E-completion records were checked separately in each boot.

Self-review copied and hashed all 100 fresh serial/harness logs into the unique
run's verified-logs directory. The verifier is retained as verify.py. Initial
broad FAIL scanning found the two optional Venus/Vulkan spawn diagnostics;
source explicitly treats these as nonfatal and parsing the captured newc ramfs
proved both programs absent. Those are NOT graphics passes; graphics and Pi/A72
KVM remain outside this result. The verifier requires exactly those two
nonfatal lines and refuses any additional failure marker.

All ten captured config/kernel/ramfs/pool/key input hashes verified. The wrapper
also compared the live immutable build inputs with the pre-matrix captures,
and source stayed clean at the recorded HEAD through completion. Stratum
remained clean at 61dde3727921e70e2c72fbd3c9e2044a192f4a54. Default bin
aa7b16dc8fdfad3b, UBSan bin 39772be897d55371; full hashes, per-boot log hashes
and checks: astra-takeover/current-gate-20261008T201816Z-result.json. Inputs
are marked PINNED outside automatic pruning. Minimum recorded disk admission
was 8758 MiB versus the unchanged 8192 MiB floor. Mac lease released.

This qualifies the current fixture on Mac default/UBSan across the five rows.
It does not expand the earlier mutant experiments beyond their recorded single
boots, nor prove final descriptor/page-table reclamation. That witness and the
remaining structural/engine obligations stay open, followed by the approved
async-first implementation then MM0-MM4. No Main landing, unfinished activation
or removal of the 128 MiB protection. This was single-agent self-review, not an
independent audit.


## October 8, 21:14: final destruction host controls

Added final-reclaim-control.py, extracting the current AddrSpace declaration,
Loom private destructor, AddrSpace lifecycle functions and all four page-table
walk/free levels directly from source. The native host run uses real malloc/free
behind checked allocator doubles, with doubles for locks, VMA drain, ring refund
and Loom layout. Actual clearing loops and the ARM dsb remain in the extracted
code. This is a sequential host boundary test, not guest allocator accounting.

On source 657343616 (kernel bytes unchanged from qualified 98c50e844), two
ASan/UBSan controls passed: owner dies before private retirement, and private
retirement happens before the remaining owner dies. Each frees exactly one
AddrSpace and four table levels, clears table bytes before freeing, excludes
block/leaf entries from recursive table freeing, and publishes retirement.
Four single-site omitted-release mutants each exited at its expected assertion:
private lifetime put or descriptor kfree omitted -> descriptor reclamation;
whole pgtable destruction or root free omitted -> four-level reclamation. No
sanitizer diagnostic was used as a substitute for those named assertions.

Raw generated sources, exact diffs, compiler commands, logs and SHA256s:
astra-takeover/final-reclaim-20261008T211401Z/. The initial 211352Z preparation
failed compilation because the host lock typedef was misspelled; its diagnostic
is retained and it made no runtime claim. Corrected before the successful run.
Self-review reread all six outputs and four diffs, checked original source and
generated-source hashes, and verified ASan dynamic linkage. Intentional mutant
leaks last until process exit; LSan is disabled, while ASan/UBSan remain enabled.
Mac lease released after 49 seconds. Disk admission exceeded 8 GiB; earlier
transient low-space readings did not cause a bake or deletion.

This closes the bounded host destruction-call sensitivity gap. Guest buddy/SLUB
reclamation, cross-CPU scheduling and actual VMA drain are NOT certified by these
doubles and remain separate obligations. No production or guest fixture changed,
so the recorded 50-boot kernel result is not superseded. Continue the approved
bounded private-engine admission/retirement work, carrying these remaining guest
obligations to its qualification; do not use host evidence as an activation waiver.
No Main landing or private async/clipboard activation, and retain 128 MiB protection.


## October 8, shared slot ledger checkpoint

Authored the approved 64-entry ring-global reservation/identity/borrow ledger,
not yet allocated or charged by Loom and not exposed to syscalls. PROVISIONAL
consumes capacity during copyout; rollback never reuses its burned incarnation.
LIVE exact-kind references may borrow, RETIRING stops new borrows, and reap waits
for zero pins. Close refuses reserve/publish/pin but preserves existing local
retirement. Internal phases are not ABI protocol states. Full engine authority,
parent-scope pins, terminal/publication gates, payload retention and accounting
remain caller obligations before retirement/reap; this table is not an engine.

Shared fixture registered as loom.service_slots (expected suite count 1837).
Actual module host ASan/UBSan control passed; 13 exact-site mutants each exited1
with precisely the expected failure, including stale generation, provisional
visibility, wrong kind, rollback generation rewind, early/live reap, capacity,
identity/pin overflow and three close-admission arms. Generated sources/diffs,
commands, hashes and raw logs: astra-takeover/slots-20261008T2130Z/. The existing
lifecycle model passed its 5828-state clean case and all seven named mutant
verdicts: astra-takeover/slots-model-20261008T2130Z/. Real ARM compiler flags
accepted module/test registration/fixture with no errors or new non-prototype
warnings. Native guest/full Mac qualification is pending for this new source.

Self-review checked exact identity before state access, no decrement of the
mint counter, caller-held-lock serialization, copyout-vs-close rollback,
pin saturation/underflow, release after close and no callback under the lock.
One important clarification to older coverage wording: loom_post_pool_cqe only
delivers an already committed result. Results committed before abort can still
be delivered; a blanket service_closing guard is not the approved repair.
Whole-ring discard requires detached consumer and local borrow retirement.
Engine integration must preserve that distinction. Self-review only, not an
independent audit. Prior 50-boot qualification remains bound to 98c50e844.
No activation, Main landing, or removal of 128 MiB protection.


October 8, native link correction: current-gate-20261008T213009Z stopped
before any boot (gate.rc=runner.rc=1). The fixture's 1552-byte aggregate zero
assignments lowered to an undefined libc memset despite freestanding flags.
Replaced those two resets with the byte-loop convention used by existing pool
and receipt fixtures. No production ledger behavior changed. Native kernel
link now succeeds; all 14 actual-module ASan/UBSan host outcomes were rerun
successfully with the corrected shared fixture (slots-20261008T2132Z/).
The original failure and original host evidence remain preserved. Full native
matrix remains pending until a new, exact-source run completes.


## October 8, 22:14 UTC: slot-ledger full Mac gate verified

current-gate-20261008T213148Z completed on exact clean source
ceac7f380334a272cbc8e37f3ac9b1923e28f761 with gate.rc=runner.rc=0.
All five standard rows (default smp1/4/8, UBSan smp4/8) passed 10/10, zero
other classifications. Each of the fifty retained serial logs independently
shows 1837/1837, loom.service_slots PASS, private-owner normal fallthrough PASS,
D7 overlap and twenty distinct d7user00..19 completed cycles, plus boot banner.
No extinction or kernel-test failure. The two optional graphics diagnostics
remain explicitly absent programs in the exact captured ramfs, not passes.

The retained verifier checked all ten captured config/kernel/ramfs/pool/key
hashes, source identity and clean end diff, and saved 100 serial/harness logs
with per-file hashes. Wrapper checks also matched live immutable build inputs
to capture. Stratum remained clean at exact 61dde37. Minimum recorded admission
space was 10757 MiB, above the unchanged8192MiB floor; Mac lease released.
Result: astra-takeover/current-gate-20261008T213148Z-result.json. The captured
input generation is PINNED. This qualifies the dormant ledger's shared fixture
on the recorded Mac source. Caller locking, native authority, scope/payload/
terminal integration and owner charging remain unimplemented here; sequential
fixture success does not close them. Guest final allocator reclamation and
Pi/A72/KVM remain open; host destruction controls remain separately bounded.
Self-review only, no independent audit. Continue private owner integration before
MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October 8: charged slot storage attached to the private owner (runtime pending)

Private construction now precharges one additional buddy page, allocates it zeroed
for the1552-byte slot ledger, and publishes it only with a fully constructed Loom.
Allocation failure and downstream layout failure free acquired storage before
refunding and dropping the exact image pin. Close freezes ledger admission under
the existing Loom lock. Final retirement requires all records FREE and unpinned,
frees the page outside locks, then follows the existing ring settlement and
metadata refund ordering. Internal users must retain their Loom reference across
slot calls/borrows. No service handler is enabled: the future engine must retire
its records before releasing its last owner reference, or the guard fails closed.
Pool storage, authority, parent scopes, terminal/payload gates and workers remain
next integration work; this is not a complete service engine.

Native linked DWARF measures Loom2864bytes (one buddy page) and the slot ledger
1552bytes (one deliberately whole-page allocation). Base private metadata is now
two pages; ring backing plus conservative Burrow descriptor attribution stays
separate. Legacy Loom storage gains one pointer but stays in its existing buddy
order. See astra-takeover/slot-owner-layout.json and retained DWARF output.

Self-review and actual-source host ASan/UBSan: slot-owner-20261008T222200Z has
8 controls (success, six refusals, busy-retirement guard) and7 exact-site mutants
for charge, unwind free/refund/pin, retirement free, close freeze and busy guard.
Every mutant fails precisely its named assertion. final-reclaim-20261008T222206Z
passes both owner-drop orders and five omitted-release controls, now including
slot-page free. Host geometry/Loom/image/lock/allocator doubles bound these
results; they are not guest physical accounting or concurrency proofs. Intentional
mutant leaks are process-bounded with LSan disabled, ASan/UBSan still enabled.

Existing lifecycle model passed5828 states and all7 named buggy verdicts
(slot-owner-model-20261008T2220Z). Real ARM syntax and native kernel linking pass.
New separate loom.private_slot_owner fixture covers allocation refusal, downstream
rollback, exact charge, close with held borrow and final refund; expected suite
count1838. Runtime/full Mac gate is still pending on this changed source; the
50-boot1837 result applies only to ceac7f380. This is self-review only. Preserve
all activation guards, Main boundary, guest final-allocator/Pi residuals and128MiB.


## October 8, 23:06 UTC: charged slot-owner full Mac gate verified

current-gate-20261008T222442Z completed with gate.rc=runner.rc=0 on exact
7c184b3ef2a1c23dbc28268abd6c12b45a3dab25, tracked source clean throughout.
All five standard rows passed10/10 (default smp1/4/8, UBSan smp4/8), with no
other classifications. The verifier independently checked all50 serial logs:
1838/1838, loom.private_slot_owner PASS, loom.service_slots PASS, private-owner
normal fallthrough PASS, D7 overlap and twenty distinct d7user00..19 completed
cycles, plus boot banner. Optional graphics programs were absent from the exact
captured ramfs; their two nonfatal diagnostics do not count as graphics coverage.

Ten captured input hashes verified, live immutable build inputs matched capture
at wrapper exit, and100 serial/harness logs were retained with hashes. External
Stratum remained clean at exact61dde37. Minimum recorded admission10930MiB was
above the unchanged8192MiB floor. Mac lease released; input generation marked
PINNED. Full result: astra-takeover/current-gate-20261008T222442Z-result.json.

This qualifies the recorded slot-page constructor/refusal/close/retirement
integration on Mac. It does not qualify future pool storage, authority, parent
scope/protocol/terminal/worker integration, guest final allocator reclamation,
or Pi/A72/KVM. The host omitted-free witnesses stay at their documented boundary.
Next is the approved pool table/member ownership and publication integration;
async lifecycle still precedes MM0-MM4. Self-review only, not independent audit.
No Main landing, private async/clipboard activation or128MiB protection removal.


## October 8, 23:22 UTC: joint pool registration and charged owner storage

The private owner now allocates a zeroed7176-byte pool table (one64-cell bank
plus64 pool descriptors indexed by the existing shared service slot). Joint
prepare reserves both identity and member capacity before output copyout.
Publish rechecks both provisional phases and rejects close without half commit.
Rollback releases both exclusions; reap refuses borrowers, streams, BUSY,
PENDING and LEASED members before modifying either ledger. Exact payload return
permits reap; neither CQ consumption nor slot removal returns a payload. The
ring-global incarnation/nonce counters never reset. All calls require the same
Loom lock and retained owner reference; backing pins are released after unlock.

Construction precharges two additional buddy pages and frees pool storage before
refund on every downstream failure. Retirement requires both tables empty and
frees them before ring settlement, metadata refund and exact image-pin release.
Native linked DWARF measures Loom2872bytes (one buddy page), pool table7176bytes
(two pages), plus the deliberately whole slot page: four metadata pages total.
Ring backing and conservative Burrow descriptor attribution remain separate.
See astra-takeover/pool-table-layout.json and retained native link/DWARF output.

Self-review: under the common lock, pool_publish cannot fail after slot_publish
succeeds because its RESERVED precondition was already checked; rollback and
reap likewise validate all fallible prerequisites before either ledger is freed.
Close blocks new registration/publication without destroying retained payloads.
The existing pre-abort committed-result delivery semantics remain intact.

Bounded ASan/UBSan host evidence: pool-table-20261008T2320Z control plus7 targeted
transaction mutants; slot-owner-20261008T231734Z nine controls plus10 charge/free/
close mutants; final-reclaim-20261008T231743Z both owner-drop orders plus6 exact
omission mutants. Every mutant failed its named assertion. The owner/final-drop
fixtures use checked allocator, geometry, image, lock and VMA doubles, so these
are call/free/order witnesses, not guest physical reclamation or race proofs.
Lifecycle model clean5828 plus7 named counterexamples and buffer models clean464/
6416 plus11 named counterexamples passed in pool-table-*-model-20261008T2312Z.

Native build/link passes. New loom.service_pool_table shares the transaction
fixture with host; loom.private_slot_owner now covers pool-allocation refusal,
downstream unwind and the full metadata charge. Expected suite count1839; the
full Mac gate is pending for this source. Prior50/50 applies only to7c184b3ef.

Still owed before activation: actual authority/resolution and canonical Burrow
pins, accepted fixed-I/O exclusion integration, parent scopes, protocol/worker/
terminal retirement and full client payload lifecycle. These helpers do not
authorize service syscalls. Guest final allocator reclamation and Pi/A72/KVM
remain open. Self-review only, no independent audit. Continue async before
MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October 9, 00:01 UTC: pool-owner full Mac gate verified

current-gate-20261008T232028Z finished with gate.rc=runner.rc=0 on exact
1295a25da8b546498661c359523917187c01d832, tracked source clean throughout.
All five rows passed10/10 (default smp1/4/8, UBSan smp4/8). Raw-log verification
confirmed1839/1839 on all50, including service_pool_table, private_slot_owner,
service_slots and private-owner normal fallthrough. Every boot passed D7 overlap
and twenty distinct d7user00..19 completed cycles plus the boot banner.
Ten captured input hashes verified and100 serial/harness logs retained with
hashes; wrapper exit also matched live immutable build inputs to capture.
External Stratum remained clean at exact61dde37. Minimum recorded admission
10974MiB exceeds the unchanged8192MiB floor. Mac lease released; inputs PINNED.
Optional Venus/Vulkan programs were absent from this ramfs, not qualified.

Evidence: astra-takeover/current-gate-20261008T232028Z-result.json and the run's
verify.py, verified-logs and input manifest. This source-specific Mac result
qualifies the pool/slot storage and transaction fixtures, not the remaining
private authority, buffer-pin/fixed-I/O exclusion, scope/protocol/worker/terminal
integration. Guest final allocator reclamation and Pi/A72/KVM remain open.
Next: registered-buffer ownership using the approved canonical Burrow and exact
creator/image rules. Async work still precedes MM0-MM4. Self-review only; no
Main landing, private async/clipboard activation or128MiB protection removal.


## October 9: internal private registered-buffer ownership (runtime pending)

loom_private_register_buffers is an internal-only admission path. It requires a
retained private Loom and creator Proc in its stable syscall image, checking both
Proc incarnation and exact retained AddrSpace before resolution. Input is an
immutable kernel snapshot; there is no new usercopy or public dispatch. Existing
ANON/writable/contiguous single-VMA resolution takes actual Burrow pins. The
ring's own backing is refused. The complete new set resolves before installation;
partial refusal leaves the prior table intact. Under the Loom lock, close or any
provisional/live pool refuses replacement. Success swaps the whole bounded table.
Speculative or displaced pins settle outside locks against the retained image,
never by rediscovering a Proc or following its post-exec address space.

Final private retirement still requires empty service/pool ledgers and no legacy
worker/operation/fid state. It now clears and settles registered-buffer pins
before freeing tables, settling the ring and dropping the retained image last.
No new allocation/charge is needed for this table: it already resides in Loom.
Pool-member canonical resolution and independent pin adoption, accepted fixed-I/O
exclusion and protocol/worker retirement remain separate unfinished integration.
Public private setup, registration and I/O remain disabled.

Self-review: resolution holds the exact image lock, installation the Loom lock,
and neither encloses pin destruction. Exec's sole-thread swap latches private
rings before replacing the image; internal callers must obey the documented
stable-syscall-image requirement. A held Loom reference prevents final table
retirement during admission. Existing AS-R9 settled drops determine refunds
inside the Burrow lifetime lock; no claim/drop/restore window is introduced.

Host ASan/UBSan private-buffers-20261009T000936Z passed control plus7 exact
authority/ring/pool/close/drop/refund mutants using checked resolution, image,
Burrow-settlement and lock doubles. The first harness attempt's wrong-image
mutant hit its resolver-lock assertion before its intended authority assertion;
that evidence is retained in private-buffers-20261009T000859Z. The corrected leg
uses an empty-table request to isolate authority. See the correction JSON.
Updated slot-owner-20261009T000729Z passes nine controls plus10 mutants;
final-reclaim-20261009T000739Z passes both owner-drop orders plus6 mutants.
Those destructor controls exercise empty buffer tables, with real buffer pins
covered separately by the new native fixture. Lifecycle model clean5828 and
all7 named counterexamples pass (private-buffers-model-20261009T0008Z).

Native compile/link passes. New loom.private_buffers covers real pin counts,
partial-resolution rollback, exact creator/image rejection, own-ring rejection,
provisional/live-pool replacement refusal, unmap-then-clear, foreign-payer final
pin release, close refusal and final retirement refund. Expected count1840;
runtime/full Mac qualification is pending for this changed source. Previous
50/50 applies only to1295a25da. This is self-review, not independent audit.
Guest final allocator reclamation and Pi/A72/KVM remain open. Continue async
before MM0-MM4; no Main landing, private async/clipboard activation or128MiB removal.


## October9: private-buffer native fixture failure and correction

The3dcff4cb9 full matrix was stopped after six completed default-smp1 boots
reproduced the same sole failure: loom.private_buffers / foreign mappings
detached,1839/1840. No source qualified. Astra stopped its own process group;
wrapper exit143 retained clean-source and input checks and released Mac. Twelve
raw serial/harness logs, hashes and exact failed inputs are retained under
current-gate-20261009T001138Z, marked PINNED as FAILED.

Cause: the new fixture mapped foreign backing at0x60000000, below the burrow
window accepted by SYS_BURROW_DETACH. The existing legacy foreign-payer fixture
explicitly documents this constraint and uses0x140000000. The correction uses
that in-window address and holds the destination image lock around the share,
as required by the mapping API. Production buffer ownership code is unchanged.
This was a fixture defect, not evidence of a platform timing problem.

A fresh rebuilt default SMP1 diagnostic passed1840/1840, including private_buffers,
pool/slot/owner fixtures, D7 overlap and twenty distinct completed login cycles,
and boot banner. Recorded source is3dcff4cb9 plus the exact fixture-only diff;
ELF/bin and input hashes are retained in
astra-takeover/private-buffers-diagnostic-20261009T001526Z. Build/input/source
stability checks passed. This one boot does not replace the full Mac matrix;
a new clean-source matrix is still required. Prior pool-owner50/50 remains
bounded to1295a25da. Self-review only; no private activation, Main landing or
128MiB removal. Continue remaining async ownership/protocol work before MM0-MM4.


## October 9: corrected private-buffer full Mac gate verified

Exact source b010ace93fae59f5bb28856d7b5456200aa8e60c passed the full
50-boot Mac HVF matrix in current-gate-20261009T001717Z: ten boots each at
default SMP1/4/8 and UBSan SMP4/8, all 1840/1840. Every boot includes
loom.private_buffers, the pool/slot/owner fixtures, D7 overlap, twenty distinct
completed d7user00..19 login cycles and the boot banner. Both runner and gate
exit zero. All 100 copied serial/harness logs and ten captured inputs were
hash-verified; see astra-takeover/current-gate-20261009T001717Z-result.json and
the run's verify.py. Captured inputs are PINNED. External Stratum is exact clean
61dde3727921e70e2c72fbd3c9e2044a192f4a54. Minimum recorded admission free space
is 10399 MiB, above the 8192 MiB floor; the runner released Astra's Mac lease.

This qualifies the corrected private registered-buffer fixture and recorded
source on Mac HVF. The failed 3dcff4cb9 run remains retained as failed evidence.
Optional Venus/Vulkan probes were absent from this ramfs; no graphics claim.
Guest final allocator reclamation and Pi/A72/KVM remain open. Canonical pool
member pin ownership, fixed-I/O exclusion and the remaining async protocol/worker
integration precede MM0-MM4. Self-review only, not an independent audit. No Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical private pool member pins (internal only)

The internal pool front-end now validates the complete immutable create record,
creator incarnation/image and receipt geometry, then resolves registered slices
to canonical Burrow identities and physical byte offsets under the Loom lock.
It uses subtraction bounds before addition, rejects ring backing and nonzero
unused/output fields, and lets the joint ledger reject overlap even through
different registered aliases or provisional pools. Every accepted member owns
one independent Burrow pin before the lock is released. No allocation or
usercopy occurs inside that lock. The buffer table remains stable while pooled.

Publish is a retained-owner continuation after future copyout. Rollback/reap
snapshot member pins under the same lock as the ledger transition; refusals
drop none. Successful removal drops the saved pins outside the lock using the
retained creator image's settled-drop primitive, safe even if buffer-table
replacement wins the unlocked gap. These wrappers must only remove pools they
created; raw ledger-only fixtures do not transfer pins to this front-end.

Self-review: source and table pins keep canonical addresses alive; no Proc is
rediscovered during removal, and no Burrow is read after dropping its pin.
One pin is released per member, including disjoint members sharing a Burrow.
Close rejects prepare and publication, but permits rollback/reap by a retained
kernel owner. The unexpected-legacy-work guard rejects inflight, admitting,
held-chain and rearm state. This is NOT the future exact fixed-I/O exclusion
implementation: private dispatch stays disabled, and accepted fixed extents
must be integrated under the same lock before activation. User authority at
continuation syscalls, copyout wiring, close/worker cleanup, protocol admission
and guest final allocator reclamation remain open. Self-review, not an audit.

Host private-pool-pins-20261009T011324Z passes ASan/UBSan control plus ten exact
mutants for image/creator, unused tail, canonical alias displacement, ring
backing, fixed admission, member pin acquisition, rollback drops, refusal drops
and final refund. Checked image/Burrow/lock doubles include table replacement
after unlock; they are sequential witnesses, not a concurrent allocator proof.
Initial harness attempts are retained: 011209Z had a misspelled ledger-close
function; 011240Z's ring-rejection mutant hit the double's zero-pin precondition
before the intended assertion. Giving the fake ring its real-world owner pin
made that mutant reach the intended assertion. Neither correction changed
production code or weakens its checks.

Native loom.private_pool_pins checks actual alias overlap, canonical offsets,
independent pin counts, provisional overlap/rebinding refusal, rollback identity
burning, stale removal, busy reap, unmap before reap, exact final table refund
and close before publication. Fresh default SMP1 diagnostic passed 1841/1841,
D7 overlap, all twenty distinct cycles and boot banner. Evidence is
astra-takeover/private-pool-pins-diagnostic-20261009T011326Z: source 1e06cc3d3
plus captured exact diff, stable rebuilt ELF/bin and six input hashes, raw logs.
Native link passed. Existing lifecycle/payload models and generic ledgers are
unchanged; their prior model results are not a proof of this new C front-end.

Full Mac qualification remains pending for the new source. The prior 50/50
remains bounded to b010ace93. Continue remaining async before MM0-MM4; no Main
landing, private async/clipboard activation or removal of the 128 MiB protection.


## October 9: canonical pool-pin full Mac qualification

Exact source 6200c9de0121abbfa08bea719f904f20ae5a9ac6 passed all 50 Mac HVF
boots in current-gate-20261009T011552Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1841/1841 including loom.private_pool_pins,
private buffers, pool/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Gate and runner exit zero. The verifier checked all
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9961 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T011552Z-result.json and run verify.py.

This is source-specific Mac qualification of internal canonical pool pins.
Exact fixed-I/O extent reservations, usercopy/authority dispatch and scope/
protocol/worker integration remain pending. Pi/A72/KVM, guest final allocator
reclamation and absent optional Venus/Vulkan probes remain unqualified.
Self-review only. Async remains ahead of MM0-MM4; no Main landing, private
async/clipboard activation or removal of the 128 MiB protection.


## October 9: internal fixed-buffer reservations and pool exclusion

Private fixed-buffer borrows now use a 40-byte node embedded in a caller-owned,
precharged request. The caller must supply an immutable kernel member snapshot
from a retained creator in its stable syscall image. Admission resolves through
the same canonical registered-buffer helper as pools, checks even provisional
pool extents under the Loom lock, and takes one Burrow pin and one Loom reference.
Pool preparation checks these accepted fixed extents under that same lock.
Touching/disjoint intervals remain legal; different registered aliases cannot
bypass exclusion. Duplicate nodes and reservations exceeding existing CQ
terminal-obligation capacity are refused. This bounds list traversal without
raising any existing limit. Actual shared terminal admission remains the future
request engine's obligation before dispatch; this helper does not dispatch.

Release requires all held/running/cancelled local readers and writers to end.
It verifies exact node membership, unlinks/clears under the Loom lock, then drops
backing and settles against the retained image before releasing the Loom ref.
Wrong-owner/double release changes nothing. Rebinding the registered table does
not lose accepted fixed pins or extents; final close cannot retire the owner
while any fixed borrower remains. The destructor fails closed if the ledger is
nonempty despite reaching zero references. No allocation, usercopy, worker wait
or final object destruction occurs inside the Loom lock.

Self-review: list/node ownership is kernel-only and exclusive to the retaining
request; no user token is trusted. Backing/owner refs are acquired before list
publication and released only after unlink. The removal path never uses the node
or Loom after its final unref. The common extent resolver avoids divergent alias
arithmetic; overlap uses half-open physical intervals and rejects invalid ranges.
Worst-case pool preparation scans at most 64 members times existing CQ capacity;
progress/latency qualification with the actual request engine remains owed.
Native DWARF reports Loom 2888 bytes (0xb48), still one charged buddy page; fixed
nodes must be included in future request-storage charges, not separately allocated
by this helper. Existing four owner metadata pages remain unchanged.

ASan/UBSan private-fixed-20261009T020251Z passes control plus ten exact mutations
for image/creator, backing/owner pins, both exclusion directions, capacity, both
release obligations and close refusal. Refreshed canonical pool owner controls
(private-pool-pins-20261009T020300Z) pass control plus ten exact mutants. Refreshed
slot-owner-20261009T020340Z passes nine controls and ten mutants, and
final-reclaim-20261009T020349Z passes both owner-drop orders and six mutants.
These use checked host doubles and do not establish concurrent protocol or
guest allocator correctness. Existing formal models are unchanged; prior model
results are not presented as a proof of the new C admission/retention path.

Fresh native default SMP1 passed 1842/1842, including loom.private_fixed, pool
pins/buffers/owner fixtures, D7 overlap, all twenty distinct cycles and banner.
Evidence: astra-takeover/private-fixed-diagnostic-20261009T020341Z captures
93db78ecd plus exact tested diff, rebuilt ELF/bin, six stable input hashes and
raw logs. The final header change only clarifies the immutable/stable-image
caller precondition. Native link passed; full clean-source Mac matrix remains
pending for this new source. Previous 50/50 is limited to 6200c9de0.

Remaining async work includes actual request/CQ admission and local-user
release wiring, target/scope/fid ownership, copyout/authority dispatch and bounded
protocol/worker retirement. No public private-service activation, Main landing,
clipboard enablement or removal of 128 MiB protection. Pi/A72/KVM and guest final
allocator reclamation remain open. Continue async before MM0-MM4; self-review
only, not an independent audit.


## October 9: fixed-reservation full Mac qualification

Exact source d3563f828652709ceb34cc3a94cddf9b42b6d9e0 passed all 50 Mac HVF
boots in current-gate-20261009T020608Z. Default SMP1/4/8 and UBSan SMP4/8
each passed ten boots, every boot 1842/1842 including loom.private_fixed,
pool pins/buffers/slot/owner rows, D7 overlap, twenty distinct d7user00..19
cycles and boot banner. Both runner and gate exit zero. The verifier checked
100 copied raw serial/harness logs and ten captured input hashes; inputs are
PINNED. Stratum remains exact clean 61dde37. Minimum recorded admission free
space was 9355 MiB, above the 8192 MiB floor. Astra's Mac lease was released.
See astra-takeover/current-gate-20261009T020608Z-result.json and run verify.py.

This qualifies the internal fixed-reservation checkpoint on Mac. Actual request/
CQ admission, target/scope/fid ownership and protocol/worker integration remain
open. Pi/A72/KVM, final guest allocator reclamation and absent optional graphics
probes remain unqualified. Self-review only. Continue async before MM0-MM4; no
Main landing, private async/clipboard activation or 128 MiB protection removal.


## October 9: native target ownership in private rings

Internal target preparation now validates one immutable 288-byte kernel snapshot,
checks the retained creator and exact image, reserves from the shared 64-slot
ledger, then resolves the actual registry fd with handle_get. The existing native
capture API enforces CWALKONLY native roots, strict 9P posts, exact registry view,
service slot and post generation; no fallback registry, peer I/O or connect
permission is introduced. Full input tails and output fields must be zero.
The retained registration transaction survives an unlocked capture/close race:
close winning before publication rolls back the slot and drops captured refs.
Provisional targets consume quota but cannot be borrowed. Copyout failure burns
the incarnation and rolls back; publish remains an internal continuation.

A 4096-byte precharged table holds 64 native 64-byte targets, indexed by the
shared identity slots rather than adding another quota. Loom is now 2896 bytes
(0xb50), still one buddy page; total owner metadata is five pages (Loom one,
slots one, pool table two, target table one). A one-shot target allocation refusal
is consumed even on invalid geometry. Every construction failure frees prior
storage before refunding the exact retained image.

Target get pins the shared live slot and Loom under the same lock. Reap refuses
borrowers and stale identities before changing either payload or slot. Successful
rollback/reap detaches the native reference under lock and destroys it outside.
Put unpins before dropping the Loom outside the lock, with no subsequent owner
access. Zero owner references therefore imply no target borrowers or retained
registration transactions. Final retirement clears remaining live/provisional
targets outside locks; scopes, fids and pools still require their own retirement.
CONNECT must still capture actual caller identity and use native admission checks.
No syscall entry or private runtime activation is included.

Self-review covered close during unlocked capture, publication, stale reference
removal, quota consumption, borrowed payload stability, final put, exact-image
charge retention and all allocation unwinds. The native target fixture uses a
real posted registry and navigation fd without a responder: closes the fd/root
and poster, proves the registry remains retained, then proves the last borrower
retires the closed ring and destroys the registry exactly once. This is an
internal owner fixture, not protocol or public usercopy qualification.

Validation: native default SMP1 passed 1843/1843, D7 overlap, twenty distinct
cycles and banner; source 0efb3c9bb plus captured diff and new target header in
astra-takeover/private-targets-diagnostic-20261009T025959Z, rebuilt ELF/bin and six
stable input hashes. Final declaration-only edits reorder fault declarations, add the test forward
prototype and clarify continuation retention. ASan/UBSan private-targets-20261009T030105Z passes
control plus twelve exact mutations; slot-owner-20261009T025921Z passes eleven
controls and thirteen mutants; final-reclaim-20261009T025933Z passes both owner
drop orders and seven mutants. Host boundaries use checked doubles and establish
neither concurrent protocol execution nor guest allocator reclamation. Two host
harness corrections and the diagnostic marker-parser correction are recorded
with untouched failed/raw logs. Full current-source Mac matrix is pending;
prior 50/50 remains limited to d3563f828.

Next: actual scope/fid/request/CQ admission, authority/usercopy dispatch, and
bounded protocol/worker integration before MM0-MM4. Pi/A72/KVM and final guest
allocator reclamation remain open. Self-review only, no independent audit.
No Main landing, private async/clipboard activation or 128 MiB protection removal.

The existing lifecycle model was rerun in private-targets-model-20261009T0303Z:
clean 5828 states and all seven named counterexamples passed. This validates the
existing finite abstraction, not the new C target authority/locking path.


## October 9: native target full Mac qualification

Exact source 4ac2790caa82653cf3a5bcf0f77aa5367c59535b passed all 50 Mac HVF
boots in current-gate-20261009T030505Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1843/1843, including native target ownership,
private fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct d7user00..19
cycles and banner. Both gate and runner exit zero. Verification retained and
hashed 100 raw serial/harness logs and all ten captured inputs; inputs PINNED.
Minimum recorded admission free space 8786 MiB exceeds the 8192 MiB floor.
Stratum remained exact clean 61dde37; Astra's Mac lease was released. Evidence:
astra-takeover/current-gate-20261009T030505Z-result.json and run verify.py.

This is Mac qualification of internal target ownership, not complete private
async support. Continue scope/fid/request/CQ admission, usercopy/authority and
bounded protocol/worker retirement before MM0-MM4. Pi/A72/KVM, final guest
allocator reclamation and absent optional graphics probes remain unqualified.
Self-review only. No Main landing, private async/clipboard activation or removal
of the 128 MiB protection. Preserve all pinned artifact generations; current
free disk is about 8.5 GiB, so recheck admission before further heavy work.


## October 9: shared scope/fid reservation metadata

The new internal loom_service_nodes module joins SCOPE/FID reservation metadata
to the existing shared 64-slot ledger. SCOPE requires a zero parent; FID requires
an exact published READY SCOPE. A child pins that parent before reserving its own
slot, including while provisional across copyout. Capacity refusal unwinds the
pin and leaves output unchanged. Rollback burns identity, clears both ledgers
and unpins the parent. Publication rechecks parent READY, so abort during copyout
wins without silently publishing a child. Ring close also rejects publication.

Only unused RESERVED nodes with zero own borrows can use reap_unused. A parent's
child pins prevent reuse, stale child refs cannot clear replacements, and child
cleanup can release its parent after close/abort. READY/ADMITTED/ABORTING/RETIRED
objects require future engine retirement with protocol/local/terminal proofs;
this helper cannot bypass them. The module does no allocation, usercopy, callbacks
or locking internally: all calls require the owner's shared ring lock and a
retained owner. Each node is 24 bytes, the table 1536 bytes. It is compiled and
fixture-tested but NOT YET allocated, charged or attached to a private Loom;
no new runtime owner storage or private syscall activation is claimed.

Self-review checked parent pin acquisition before slot publication, quota-failure
unwind, exact parent/child incarnations, provisional invisibility, parent abort
versus child publish, and refusal-before-mutation on reap. The shared native/host
fixture explicitly simulates parent READY/ABORTING/RETIRED: it is not a handshake,
authority, concurrent-lock or final-protocol-retirement witness.

ASan/UBSan nodes-20261009T0355Z passes control plus eleven exact mutants; its input
hashes include both the new module and actual shared slot source/header. The
initial nodes-20261009T0352Z also passed, before expanding provenance coverage.
Existing lifecycle model nodes-model-20261009T0355Z passes clean 5828 states and
seven named counterexamples; it is the existing abstraction, not a C proof.
Fresh default SMP1 passed 1844/1844, D7 overlap, twenty distinct cycles and banner.
Evidence nodes-diagnostic-20261009T035230Z retains source579329177 plus exact diff,
rebuilt ELF/bin, six stable input hashes and raw logs. Full current-source Mac
matrix remains pending; previous 50/50 is limited to target source4ac2790ca.

Next: integrate charged node storage and unused-node close cleanup into the
private Loom, then scope/fid protocol resources, request/CQ admission, usercopy
and bounded worker progress; continue async before MM0-MM4. Preserve the 128 MiB
protection and disabled private async/clipboard; no Main landing. Self-review
only, no independent audit. Pi/A72/KVM and final guest allocator reclamation
remain open. To preserve disk headroom, 700 duplicate build archive logs were
removed only after byte/hash equality with retained canonical verified logs;
log-cache-reclaim-20261009T0355Z.json records every pair. No pinned input or
canonical raw evidence was removed.


## October 9: scope/fid metadata Mac gate verified

Exact source c5abd02647e9744eb200afe65e4530b19b5cd5af passed all 50 Mac HVF
boots in current-gate-20261009T035529Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passed 1844/1844, including loom.service_nodes and the
existing target/fixed/pool/buffer/owner fixtures, D7 overlap, twenty distinct
d7user00..19 cycles and banner. Gate and runner exited zero; raw verification
retained and hashed 100 serial/harness logs and all ten captured inputs. Inputs
are PINNED. Minimum recorded admission free space was 8282 MiB, above 8192.
Astra's Mac lease was released. Result: astra-takeover/
current-gate-20261009T035529Z-result.json and the run's verify.py.

This verifies metadata with explicitly simulated parent protocol states. Charged
node owner storage, actual scope/fid protocol resources, request/CQ admission,
usercopy/authority and worker retirement remain next, before MM0-MM4. Pi/A72/KVM,
final guest allocator reclamation and absent optional graphics probes remain
unqualified. Self-review only; no Main landing, private async/clipboard activation
or removal of the 128 MiB protection. Preserve pinned artifact generations.


## October 9: charged node owner and unused-node retirement

Private Loom now owns the 1536-byte SCOPE/FID metadata table in a separately
allocated, zeroed 4096-byte buddy page. Charge is reserved before allocation;
constructor failures free node/target/pool/slot storage before refunding and
dropping the exact image pin. The node allocation fault is consumed even by
early geometry refusal. Loom is 2904 bytes (native DWARF 0xb58), still one buddy
page: metadata is now six pages including slot, pool, target and node storage.
No syscall ABI or private activation changes.

At zero owner references, the retire worker has exclusive access. It cleans
unused FIDs before unused SCOPEs, using exact rollback for provisional nodes
and unused reap for live nodes; child cleanup releases its parent pin first.
Only RESERVED nodes with no own borrows qualify. READY/ADMITTED/ABORTING/RETIRED,
invalid identities and orphan payloads fail closed: actual protocol retirement
is still the engine's obligation. Node storage is freed before the final image
refund/private_end, outside locks. There is no peer I/O, wait or usercopy here.

Self-review checked whole-page charging against allocation size, all unwind
edges, last-owner exclusivity, child-before-parent ordering, stale/borrow/state
guards and final image lifetime. The native owner fixture checks node allocation
refusal, zeroed storage, complete charge, automatic provisional/live unused-scope
cleanup and restoration of image charge/refcount. Host control explicitly
simulates parent states for child cleanup; this is not a protocol witness.

Evidence: slot-owner-20261009T044245Z passes 19 controls and 18 exact ASan/UBSan
mutants, including reversed cleanup order, omitted cleanup, charge/free/unwind
and orphan guards. Initial slot-owner-20261009T043850Z passed 19 controls and
16 mutants before the two extra sensitivity checks. final-reclaim-20261009T043907Z
passes both owner-drop orders and eight omission mutants at the host allocator
boundary. Native link succeeded; node-owner-diagnostic-20261009T044103Z passed
1844/1844, D7 overlap, twenty distinct cycles and banner, source cfa1c5c63 plus
its captured diff with six stable input hashes and retained ELF/bin. Existing
lifecycle model node-owner-model-20261009T0442Z passes clean 5828 states and
seven named counterexamples. Full Mac coverage for these owner changes is
pending; prior 50/50 belongs to c5abd0264. No independent audit is claimed.

Next: actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress, then MM0-MM4. Private async and
clipboard stay disabled; keep the 128 MiB protection and do not land Main.
Pi/A72/KVM, final guest allocator reclamation and optional graphics remain open.

Disk admission headroom was restored by reclaiming five idle, ignored, untracked
native Cargo debug caches under the operator resource grant, from Main and old
Aux checkouts. No source, release outputs, run evidence or pinned generation was
removed. native-debug-cache-reclaim-20261009T0443Z.json inventories exact paths
and files; measured free space rose from 8164 to 8737 MiB. Full gate admission
will independently enforce the unchanged 8192 MiB floor.


## October 9: charged node owner full Mac gate verified

Exact source 262ff29d3fe306dd700481f36d1fa4dd40aaede0 passed 50/50 Mac HVF
boots in current-gate-20261009T044430Z: default SMP1/4/8 and UBSan SMP4/8, ten
each. Every boot passed 1844/1844, including charged private owner/unused-node
cleanup and existing node/target/fixed/pool/buffer fixtures, D7 overlap, twenty
distinct login cycles and banner. Both exits were zero; verified 100 raw logs
and all ten input hashes, and marked inputs PINNED. Minimum recorded admission
free space was 8208 MiB, above the unchanged 8192 floor. Mac lease released.
Result: astra-takeover/current-gate-20261009T044430Z-result.json.

This is internal owner/metadata coverage, not complete private async support.
Actual scope/fid protocol resources and retirement, request/CQ admission,
usercopy/authority and bounded worker progress remain next before MM0-MM4.
Pi/A72/KVM, final guest allocator reclamation and absent optional graphics probes
remain unqualified. Self-review only. Keep private async/clipboard disabled,
128 MiB protection and all pinned evidence; no Main landing. Current free disk
is only about 8 GiB: restore headroom before the next build/gate without lowering
admission or removing canonical evidence.


## October 9, 05:30 UTC: next integration boundary and disk headroom

Readback of the existing APIs confirms that devsrv_service_prepare owns an
unpublished connection/create ref and registry view; devsrv_connect_publish
may run under the owner admission latch and wake runs after unlocking. The
existing p9_client_init_preallocated/progress_bind/progress_step/progress_abort
path supplies exclusive, nonblocking handshake/request mechanics. Binding these
to a scope must first reserve its terminal obligation and all charged storage,
then serialize backlog publication with close/abort. Retained creator stripes
and exact AddrSpace feed proc_service_snapshot_by_stripes; no worker identity
or blocking legacy handshake/close is permitted. This readback is not an
implemented or tested scope binding. Continue that integration before MM0-MM4.

Additional idle host Cargo debug caches under build/usr-rs/debug were reclaimed
in Main, b2c, Aux-kaua and Aux-hunt under the resource grant. All were ignored,
untracked and unused by any cargo/rustc process; no source, release output,
canonical evidence or pinned input was removed. Exact inventory and process
snapshot: astra-takeover/host-debug-cache-reclaim-20261009T0530Z.json. Actual free
space rose 8217 -> 8441 MiB (APFS sharing limits logical-byte savings). Recheck
headroom before the next heavy build; keep the 8192 MiB admission floor.

## October 9 05:42 UTC: scope/FID owner transactions

Implemented internal creator/image-bound prepare/publish/rollback/unused-reap
entry points over the existing charged tables. Immutable 48-byte record
validation and same-Loom-lock publication preserve provisional visibility,
parent pins and burned identities across copyout failure, abort and close.
Caller retains the Loom across every continuation; no protocol or syscall
activation. Self-review performed (not independent).

Verification: extracted actual-C ASan/UBSan control plus 13 expected named mutant
failures (private-nodes-20261009T053849Z); native diagnostic
node-transactions-diagnostic-20261009T053938Z passes 1845/1845, D7 overlap,
twenty distinct d7user00..19 cycles and boot banner, with stable source diff,
added fixture hash and six input hashes. Lifecycle model clean5828 and all seven
named counterexamples pass (node-transactions-model). Full current-source Mac
matrix remains pending; prior 50/50 applies only to262ff29d3.

Operator cleanup removed work/oct5-as-r9 while this turn started. With explicit
operator permission, all2698 tracked files were restored exactly from HEAD;
large untracked artifacts were not restored. Historical logs/results remain,
but old untracked pinned inputs and flight JSON files are absent locally; do
not claim those historical binaries can currently be rechecked. Current build
inputs survived and a new diagnostic retains fresh ELF/bin clones. Disk is now
above16GiB; floor remains8192MiB. Astra's six pending files and four protected
drafts verified unchanged; Stratum remains clean61dde37.

Next: complete the current-source gate, then charged request/terminal records
before native connection publication, actual scope/protocol ownership, bounded
progress and local retirement. AS-3/4 and then MM0-MM4 follow. Main landing,
private async/clipboard activation and128MiB removal remain unauthorized here.

## October 9 06:28 UTC: scope/FID transactions full Mac gate verified

Exact source `d4825ae1e106924d67d3eb405180da7f8d5882d6` passed
`current-gate-20261009T054208Z`: 50/50 boots, default SMP1/4/8 and kernel
UBSan SMP4/8, ten each. All boots passed1845/1845 including the new
`loom.private_node_owner` fixture, D7 overlap, twenty distinct d7user00..19
login/logout cycles and the boot banner. Both gate.rc and runner.rc are0.
The verifier retained and hashed100 raw serial/harness logs and checked all ten
captured input hashes against the clean source/end-source and Stratum61dde37
provenance. Lowest recorded admission headroom16194MiB;8192MiB floor intact.
The runner released Astra's Mac lease. Result:
`astra-takeover/current-gate-20261009T054208Z-result.json`; verifier beside run.
Astra's pending files and four protected drafts, plus clean Stratum pin, were
rechecked unchanged after the run. This is self-verification, not an independent
audit. Optional graphics probes remain absent from this exact ramfs; Pi/A72/KVM
and final guest allocator reclamation remain unqualified.

This qualifies the internal reservation owner checkpoint only. Next: charged,
bounded request records and terminal-completion obligations before native
backlog publication, then actual scope/protocol binding, caller authority,
bounded progress and local retirement. Private service registration/CONNECT and
clipboard remain disabled, Main untouched,128MiB protection retained. MM0-MM4
still follow completion of async integration and its client/consumer gates.

## October 9 06:53 UTC: request/terminal owner checkpoint

Continue autonomously through checkpoints, per the operator's latest explicit
direction; no handoff/confirmation between verified implementation steps.

Added the bounded request ledger (56-byte records,32-byte table descriptor),
CQ-sized precharged buddy storage on private Loom, allocation-fault unwind and
unused-only zero-owner cleanup. Each record owns an exact scope pin and monotone
request identity. Terminal commit requires local borrow count zero, rejects
fresh success after abort/close, and preserves prior committed success across
CQ pressure. Delivery and storage reap are separate. Detach/discard checks do
not replace the engine's actual local-quiescence proof. Actual private terminal
posting uses kernel CQ geometry, retains full-CQ obligations, clears receipts
and wakes after unlock. Private syscall activation remains off.

Native diagnostic request-owner-diagnostic-20261009T064808Z:1847/1847, new ledger
and owner/CQ fixtures, D7 overlap +20 distinct cycles +banner; exact source diff,
three added-source hashes and six inputs stable. Native Loom size2936(0xb78),
one page; small-ring total metadata now7pages. ASan/UBSan: ledger1+13mutants,
post1+6mutants, owner27controls+23mutants including128/8192-entry geometry and
budget refusal, final-reclaim both orders+9mutants (host boundary only).
Lifecycle clean5828/seven expected counterexamples pass. Initial harness-only
unused-parameter/stale-free-count failures preserved and corrected. Self-review
performed; no independent audit. Full current-source gate pending; previous
50/50 belongs only to d4825ae1e.

Next continue directly: reserve real per-scope protocol storage and caller
identity, bind native devsrv prepare/publish + preallocated p9 progress to these
request/terminal owners, wire bounded progress and retirement, then AS-3/4 and
MM0-MM4. No Main landing, private activation, clipboard activation or128MiB
removal. Keep8192MiB floor and clean external Stratum61dde37; preserve protected
Astra files and operator cleanup. All earlier completed monitors stay paused.


## October 9 07:46 UTC: request gate verified; charged protocol preparation

The request/terminal owner source45d26893bd8b5e16a3934eed008ae4804f5bff38
passed all50 Mac HVF boots in current-gate-20261009T065820Z: default SMP1/4/8
and kernel UBSan SMP4/8, ten each. All50 passed1847/1847 including request
ledger/private CQ posting, D7 overlap, twenty distinct d7user00..19 cycles and
banner. Both rc0; verifier retained100 raw logs and verified ten captured input
hashes, clean source/end-source and clean Stratum61dde37. Minimum recorded
admission15572MiB, floor8192 intact. Result:
astra-takeover/current-gate-20261009T065820Z-result.json. Mac lease released.

Continued directly into real prepublication protocol ownership. The new internal
loom_service_protocol owner borrows an exact registered target, snapshots the
live creator's stripes plus exact pinned image, prepares a fresh native SrvConn
with ordinary weighted credits, and replaces the target borrow with its own
Loom reference. It reserves the complete image page charge before allocation,
then initializes the existing allocation-free client and exclusive native try
adapter. Tversion is queued locally; no backlog publication or byte I/O occurs.
Close is rechecked before returning ownership. Future CONNECT publication must
still reserve the request/destination first and recheck deadline/abort under Ll.

Native owner size42208 bytes (DWARF0xa4e0), rounded to16 buddy pages. Both RX and
TX are separate class-sized allocations, disjoint from the client as required
by the existing API: normal32KiB pays32 pages total, bulk128KiB pays80. This is
additional to ring metadata and SrvConn's weighted credits, not MM0-MM4 global
physical accounting. No bulk degradation or uncharged cache allocation at init.
Exclusive unused release refuses a published connection unchanged. Otherwise it
aborts/destroys the local cursor, releases native ownership, frees TX/RX/owner,
refunds the retained exact image and drops Loom LAST, all final drops outside
Ll/as locks. This cannot certify retirement of an admitted scope or live borrows.

Self-review (not independent) is preserved in protocol-draft/self-review.txt.
It caught and corrected an initial inline-TX alias against the preallocated
API's disjoint-buffer contract before integration. Final actual-owner-C host
ASan/UBSan evidence control-5 passes20 controls and11 named mutants, including
that alias, all charge/free/ref edges, late close and published-cleanup refusal.
Its boundary doubles are not a wire or guest physical-reclamation proof.

Fresh native diagnostic protocol-owner-diagnostic-20261009T074413Z passes
1848/1848, the actual native protocol-owner fixture on both buffer sizes,
allocation/charge refusal, actual creator capture, absence of peer effects,
unused cleanup and final Loom/image retention, plus D7 overlap/twenty distinct
cycles/banner. Exact HEAD+diff, new source/header hashes, six input hashes and
rebuilt ELF/bin retained and stable. This newer implementation still owes its
full matrix; prior50/50 is limited to45d26893b.

Failures retained and resolved: the initial native link emitted a freestanding
memset libcall for the large fixture initializer; explicit byte-zeroing repaired
it. Native diagnostic074015Z then failed1847/1848 because a bare proc_alloc
fixture is absent from the kproc-rooted production identity walk. The fixture
now uses existing proc_test_link/unlink and checks a positive identity snapshot
before negative cases; production authority checks were not weakened. Initial
Apple clang17 host ASan startup stalled before main; a retained process sample
shows recursive sanitizer/dyld/malloc initialization, and the established LLVM22
plus explicit SDK toolchain passes. No failed evidence was discarded.

Continue autonomously through the new full gate and then bind prepared protocol
ownership to exact scope/request publication, bounded executor progress and
actual local retirement, followed by AS-3/4 and MM0-MM4. Do not rerun the completed
protocol-draft installer/supervisor: their historical failure and manual recovery
are recorded; canonical implementation is now in kernel/. Protected Astra files
and clean Stratum pin rechecked. Private activation/clipboard remain disabled,
Main untouched,128MiB protection retained. Pi/A72/KVM, final guest allocator
reclamation and absent optional graphics remain unqualified. Self-review only.

## October 9: full protocol-preparation gate verified after TUI migration

Source fa7e6c5bcdf0d8bdffecdbca7720f95e3cd1d20f passed all 50 Mac HVF boots
in current-gate-20261009T075147Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. Every boot passed 1848/1848 including loom.private_protocol,
D7 overlap, twenty distinct login/logout cycles and boot banner. Both runner
and gate exited zero. Verified 100 raw serial/harness logs and ten captured
input hashes; complete generation now PINNED. Minimum recorded admission
14708 MiB, above the unchanged 8192 MiB floor. Results are in
astra-takeover/current-gate-20261009T075147Z-result.json.

The TUI initially permitted only read-only access to these project checkouts;
operator restored full access and the same verifier has now retained the
canonical evidence and completed flight record. No new implementation or gate
ran during that access interruption. The CONNECT draft remains uninstalled
and unqualified. Continue scope/request publication, bounded progress and
local retirement, then AS-3/4 and MM0-MM4. This gate qualifies charged
unpublished preparation only, not active private runtime. No Main landing,
private async/clipboard activation or 128 MiB protection change. Self-review
only; no independent audit.

## October 9: CONNECT ownership, publication and local retirement

Astra bound charged protocol ownership to an exact RESERVED scope and a
precharged CONNECT terminal obligation. The request and protocol hold separate
scope pins before native allocation; duplicate scope preparation is refused.
Native backlog publication and request admission share the Loom lock with
cancellation and success. No bytes are sent before publication. One driver
uses the real nonblocking 9P client outside that lock, retaining a parser borrow.
READY requires actual version/attach completion. The CONNECT deadline ends at
committed success; later cancellation preserves that success for CQ delivery.

Abort acknowledgement latches transport shutdown; local retirement additionally
requires cursor abort and no concurrent teardown users. A full CQ retains the
terminal and protocol storage. The CONNECT owner alone reaps its delivered
request, drops the other scope pin, destroys local storage, refunds the exact
image and drops Loom last. Explicit metadata reap still requires no child pins.
A server-retained endpoint keeps its own native lifetime and weighted credits.
Protocol failures use EIO and record the attempted handshake phase as required
by the existing ABI; timeout/cancellation preserve their distinct reasons.

Final actual-C host ASan/UBSan evidence connect-control-20261009T170708Z passes
11 controls and 12 named mutations, including abort during progress, a second
driver, a teardown user delaying retirement, full CQ, and recorded failure phase.
These use checked native/protocol doubles, not a wire or SMP stress proof.
Existing protocol owner controls pass20+11 mutations, and the request ledger
passes its control plus13 mutations after factoring the nonmutating admission
check. Lifecycle model passes5828 states and seven expected counterexamples.
Self-review is in connect-draft/self-review.txt; no independent audit. It caught
and repaired the missing protocol failure phase before this checkpoint. Initial
host-runner mutation-count and unused-stub failures are retained; production
controls passed, the harness was corrected and all named mutations now fail at
their intended assertions.

Fresh native connect-diagnostic-20261009T170730Z passes1849/1849 including
loom.private_connect, real normal/bulk version+attach, error/cancel/timeout,
CQ backpressure and server-retained endpoint cleanup; D7 overlap, twenty distinct
login/logout cycles and banner also pass. Exact source diff/new-source hashes,
six input hashes and rebuilt ELF/bin are retained and stable. The earlier
170510Z diagnostic passed before adding failure-phase coverage and stays bounded
to that earlier source. The new full Mac matrix is pending; previous50/50 still
qualifies only fa7e6c5bc protocol preparation.

Continue autonomously: per-ring CONNECT lookup/entry lifetime and bounded fair
progress, close/detach retirement queue and query/reap integration, then remaining
private ops/provided buffers, AS-3/4 and MM0-MM4. This internal engine still has
an explicit caller-lifetime contract and permits only the handshake; no external
RPC/fid/payload borrow may escape it. No syscall/ring dispatch or private async/
clipboard activation, Main landing, or128MiB protection change. Preserve clean
Stratum61dde37, Astra pending/protected work, and8192MiB runner floor.

## October 9: CONNECT full Mac gate verified

Source b2b263e3f250ecaa42c7975a86beec7b65128f12 passed50/50 Mac HVF boots
in current-gate-20261009T170944Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. All boots passed1849/1849 including loom.private_connect, D7 overlap,
twenty distinct login/logout cycles and boot banner. Gate/runner rc0; verified
100 raw serial/harness logs and ten input hashes, with the complete generation
PINNED. Minimum recorded admission 13185MiB exceeds the8192MiB floor.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T170944Z-result.json.

This qualifies internal CONNECT publication/handshake/local retirement at that
source. The separate owner-index draft remains native-unqualified until installed
and tested. Pi/A72/KVM, guest allocator reclamation and absent optional graphics
remain unqualified. Self-review only; no independent audit, Main integration,
private async/clipboard activation or128MiB protection change. Continue the
per-ring index checkpoint, then close queue/executor and AS-3/4/MM0-MM4.

## October 9: indexed owner native checkpoint

See astra-takeover/pickup-latest.md for the compact current state. New indexed
CONNECT ownership, charged private page and exact lookup/pin/collection pass
native1850/1850+D7/20cycles in engine-diagnostic-20261009T1752Z; full matrix
pending. Initial1751Z metadata fixture expectation corrected with failure
preserved. Actual-C controls/mutants,89-state index model/six counterexamples,
5828-state lifecycle/seven counterexamples and self-review retained. Close queue
and executor next; no private activation, Main or128MiB change.


## October 9: full owner-index Mac gate verified

Source a978d19c388f512a645cdcbaeea60a81a0a06727 passed50/50 Mac HVF boots in
current-gate-20261009T175812Z: default SMP1/4/8 and kernel UBSan SMP4/8,
ten each. Every boot passed1850/1850 including loom.private_engine, D7
overlap, twenty distinct login/logout cycles and the banner. Both rc0;
100 serial/harness logs and ten input hashes verified and retained/PINNED.
Minimum recorded admission 11875MiB, above8192MiB.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T175812Z-result.json.

This qualifies the internal charged index and explicit progress at that source.
The separate close queue draft still owes native and full-matrix verification.
Self-review only; no independent audit, Main landing, private async/clipboard
activation or128MiB protection change. Continue close queue, active executor,
remaining private ops/provided buffers, AS-3/4 and MM0-MM4.


## October 9: private close queue and real owner retirement

Installed bounded reference-owning close queue, nonspinning dependency wakes,
physical fixed-buffer settlement barrier and exact retired-scope cleanup.
Actual-C ASan/UBSan queue/engine/wrapper/fixed/metadata/constructor/final-free
controls and targeted mutations pass; model18 states/seven counterexamples.
Self-review only, preserved in close-draft/self-review.txt.


Fresh native close-diagnostic-20261009T183912Z passes1851/1851, including
all seven close cases and second-ring progress while the first is parked. D7
overlap, twenty distinct login/logout cycles and boot banner also pass. Rebuilt
ELF/bin, six input hashes, exact HEAD+diff and new queue source/header hashes
are retained and stable. Real close/exec, full CQ, retained entry/fixed/constructor
owners and installer/driver/teardown interleavings finish local retirement with
exact image refunds while server endpoints remain retained. Source-matched Mac
SMP/UBSan matrix for this new code is still pending; the earlier50/50 qualifies
a978d19c3 index-only source. Pi/A72/KVM, final guest allocator reclamation and
optional graphics remain unqualified. No private activation/Main/128MiB change.


## October 9: CLOSE-R1 physical rollback ordering repair

Self-review found and reproduced a premature close completion: unpublished
CONNECT rollback removed its PREPARED request and scope pins before physical
cursor/storage destruction. References prevented final free, but a concurrent
close worker could detach and declare DONE too soon. Rollback now retains the
request and both pins through destruction outside Ll, then drops them under Ll.
The public wrapper retains a temporary Loom root through cleanup and wake.

Actual-C ASan/UBSan reproduction rejects the old order and accepts the repair.
Canonical CONNECT11 controls/12 mutants and close-engine18 controls/7 mutants
pass. Native rollback-native-20261009T1854Z passes1851/1851, D7 overlap,20 distinct
login/logout cycles and banner, with source/input hashes and rebuilt ELF/bin.
The extended native close fixture forces a real retirer visit while unpublished
protocol destruction is suspended and requires the barrier still retained.
Astra deliberately stopped the5ab15c4d5 full matrix with SIGTERM(rc143), preserving
16 partial serial/harness logs. This was an intentional repair stop, not a full
qualification. The repaired source requires a fresh full matrix. Review and
raw evidence: astra-takeover/rollback-draft; self-review only, no independent
audit. No Main/private async/clipboard activation or128MiB protection change.


## October 9: repaired close lifecycle full Mac qualification

Exact source ba2ae6df3dd815a4853985cc81e9f981ca0d1bdb (CLOSE-R1 implementation
9b6672f7d) passed50/50 Mac HVF boots in current-gate-20261009T185605Z:
default SMP1/4/8 and UBSan SMP4/8, ten each. All50 pass1851/1851, including
actual close/exec, retained owners, fairness and the forced physical rollback
boundary, plus D7 overlap,20 distinct login/logout cycles and banner.
Both exits zero;100 raw serial/harness logs and ten input hashes verified and
retained/PINNED. Minimum recorded admission10930MiB, floor8192 preserved.
Mac lease released. Result: astra-takeover/current-gate-20261009T185605Z-result.json.

This qualifies that repaired internal close source. The separate query/reap
and retained-diagnostic draft passes nine host scenarios and fifteen mutations,
but is not installed or native-qualified. Continue those controls and active
readiness executor, remaining operations/buffers, AS-3/4, then MM0-MM4.
Pi/A72/KVM and final guest allocator reclamation remain unqualified. Self-review
only; no Main/private async/clipboard activation or128MiB protection change.


## October 9: internal service query, abort and reap controls

Creator stripes and exact address-space image authorize copied-record internal
QUERY/ABORT/REAP. Validation rejects malformed/output-only inputs and stale
incarnations without changing query output. A locked snapshot reports TARGET,
SCOPE or FID state and exact parent identity; POOL retains its dedicated query.
Reap dispatch carries only exact identity, with each consuming primitive
rechecking state and pins under Ll and making final drops outside Ll.

The existing charged node page now stores40-byte entries,2560bytes total within
its4096-byte allocation. First reason, attempted failure phase, sent-byte state
and actual local retirement survive protocol/index collection. QUERY counts
PREPARED/ACTIVE obligations in active_ops and unpublished TERMINAL obligations
in pending_terminals. DELIVERED storage awaiting collection counts neither;
its pins still prevent reap. Query never races an unlocked protocol cursor:
completed driver diagnostics publish under Ll, while an active driver or parser
borrow conservatively reports possible escaped bytes.

ABORT remains independent of CQ capacity and preserves a committed success.
An unindexed constructor gets an admission-closing node latch; its publication
and driver reentry observe it. Rollback retains CLOSE-R1's physical destruction
barrier before marking diagnostic retirement. Unused scopes retire immediately
only with no pins; indexed owners still require actual cursor/borrow quiescence.
Scope metadata survives collection until children and outstanding ownership are
gone and explicit exact reap succeeds. Published runtime ownership must use the
index; these copied-record helpers do not install syscall/ring dispatch or an
active executor. No general RPC/FID protocol or private userspace activation.

Nine actual-C host scenarios and15 named mutations pass under LLVM22 ASan/UBSan.
Canonical CONNECT11+12mutants, node transactions, constructor29+26mutants and
final-free both orders+10mutants pass after installation. Evidence records the
initial child-pin boundary-double correction, earlier expected-mutation-message
mismatch and missing draft test include path; production controls were preserved.
Self-review: astra-takeover/control-draft/self-review.txt, not independent audit.

Native control-native-20261009T2027Z passes1852/1852, D7 overlap,20 distinct cycles
and banner. Real normal/bulk handshakes behind full CQ preserve success through
abort; retained child pins block reap; diagnostics survive physical collection;
accepted server endpoints survive local refund. The actual unused-destruction
boundary still reports ABORTING until cleanup completes. Exact HEAD+diff, new
source hashes, six stable inputs and rebuilt ELF/bin retained. Full Mac matrix
for these changes is pending; prior50/50 qualifies ba2ae6df3 close-only source.
No Main/private async/clipboard activation or128MiB protection change.


## October 9: internal controls full Mac qualification

Exact source ade9c69d17e20a5aa5ac337bfef15d7a5e2304c4 passes50/50 Mac HVF
boots in current-gate-20261009T203019Z: default SMP1/4/8 and UBSan SMP4/8,
ten each. Every boot passes1852/1852, including private controls and repaired
close lifetime, D7 overlap,20 distinct login/logout cycles and boot banner.
Both exits zero. Verifier preserved100 raw logs and checked ten pinned input
hashes. Minimum recorded admission9604MiB;8192MiB floor preserved.
Result: work/oct5-as-r9/astra-takeover/current-gate-20261009T203019Z-result.json.

The readiness/executor draft remains separate and not native-qualified. Its
actual-C readiness14/17mutants, worker5/8mutants, engine18/7mutants, CONNECT,
constructor and final-free regressions plus ARM64 syntax pass. EX-W1 missed
scan-pin-return wake was reproduced and repaired; raw failure retained. Existing
poll/lifecycle/index model controls and counterexamples pass, with no claim
that they prove the new executor. Next install the reviewed draft, rebuild and
run fresh1853 native before its own exact-source matrix. Self-review only;
no Main/private user async/clipboard activation or128MiB protection change.


## October 9: readiness-aware internal private executor

The existing per-ring SQPOLL thread now has an internal private handshake
executor. Creator stripes and the retained address-space image supply the
existing thread-budget ticket; no asynchronous raw Proc pointer is retained.
Ll serializes the start token across charge/allocation/publication or rollback.
A failed start frees its never-readied thread before refund and token release.
Ordinary private SQPOLL setup remains refused; no user dispatch is activated.

Readiness visits scan at most64 entries and drive one actionable owner with
the shared round-robin cursor. Cursor state is sampled under Ll only while no
driver/parser user is active; real protocol steps recheck state and never wait
for a peer. Native poll directions follow handshake phase. An exclusive wait
sweep retains entry pins, registers native interests before checking readiness,
then sleeps on atomic changes, CQ head, poll flags and the earliest absolute
ACTIVE deadline. READY discards its CONNECT timer; full-CQ terminals park.
The Rendez predicate never takes Ll or a device lock. Embedded64 poll hooks
fit within the existing charged4096-byte index page, checked on actual ARM64.
Every hook is unregistered before any wait pin is returned.

The executor borrows the ring lifetime. The zero-reference retirer wakes, joins
and frees the actual worker before ticket refund or table/storage destruction.
A last protocol unref from the worker queues this cleanup; it never self-joins.
Handle-close DONE includes start rollback and actual executor exit. Exit
publishes EXITING/exited in the existing IRQ-masked handshake before queue/join
wakes. Local completion is peer-independent but needs scheduler service.

Self-review found EX-W1: a silent readiness-scan pin return could strand a close
or armed collector. The actual-C witness rejects that draft; repair samples the
dependency and decrements its pin in the same Ll interval, then wakes unlocked.
Fourteen readiness cases/17mutants, five worker cases/8mutants, engine18/7,
CONNECT11/12, constructor29/26 and final-free both orders/10mutants pass.
Actual ARM64 syntax passes. Existing poll/lifecycle/index model controls and
counterexamples pass; they do not prove the new executor or native scheduler.
Self-review and failed witnesses: astra-takeover/executor-draft/self-review.txt.

Native executor-native-20261009T2116Z passes1853/1853, D7 overlap,20 distinct
login/logout cycles and banner with stable HEAD+diff, eight reviewed source
hashes and six input hashes. Real normal/bulk peers exercise registration-window
response injection, stalled-peer fairness, full CQ/drain, timeout without a new
edge, handle-close exit and exact ticket/image refund. An empty live parked
executor is joined by zero-reference cleanup. The2114Z capture attempt failed
before VM launch on an obsolete filename; repaired from the reviewed manifest,
with failure retained. Full new Mac matrix is next; ade9's50/50 qualifies only
the older1852 controls source. Self-review only, not independent audit.

General RPC/FID operations, provided-buffer dispatch and usercopy remain ahead
of AS3/4 and MM0-MM4. No Main/private user async/clipboard activation or128MiB
protection change. Pi/A72/KVM and final guest allocator reclamation remain open.


## October 10: private operation storage and callback capture prerequisites

The request ledger now separates its terminal obligation from physical RPC
storage ownership. Its56-byte record reuses a reserved field for a single owner:
Ll admits that owner only while PREPARED, and rollback/reap refuse reuse until
physical destruction. Completion delivery/discard does not release storage.
ACTIVE/borrowed, stale and duplicate owner drops fail; existing CONNECT uses0.

The internal operation constructor retains an exact indexed READY protocol and
the creator image through allocation and destruction. It reserves the terminal
and physical owner under Ll before charging and allocating one full buddy page.
Close/abort is rechecked after allocation. Refusal and exclusive unused cleanup
free storage, refund the image, release exact request/entry pins, then drop the
Loom last. No allocator, image/client lock, wake or final drop nests under Ll.
The constructor does not admit or submit an RPC and cannot free an active one.

Copied-SQE shape checks preserve all64-bit incarnations, reject reserved fields
and malformed fixed/pool combinations, and validate clone/component shape.
They grant no authority. Reply capture retains pointer-free metadata and copies
bounded payload before callback parser storage can be reused. It never allocates,
locks, publishes CQ or commits success; future driver admission and abort/deadline
checks remain mandatory. Malformed type/fid/count/walk/stat shape fails closed.

Host checks: shape1832cases/13mutants, reply capture13mutants, request physical
owner9mutants plus all17 existing request mutants, and charged constructor10
mutants pass under LLVM22 ASan/UBSan. ARM64 syntax and actual kernel link pass.
CAP-L1: native link first caught large struct assignments emitting unavailable
libc memcpy; explicit byte copying repaired it, and all capture mutants reran.
The first linker log and earlier harness failures remain in operations-draft.

Native operation-storage-native-20261009T215931Z passes1854/1854, D7 overlap,
20 distinct login/logout cycles and boot banner. Real normal/bulk READY peers
exercise expired admission, actual image quota and allocator refusals, exactly
one-page charge/refund, terminal-capacity refusal, monotone request identity,
and close PARKED until physical operation ownership is released, then DONE.
Eleven reviewed source hashes, six stable inputs, HEAD+diff and rebuilt ELF/bin
match that run. This is bounded CPU1 native storage evidence; f719ee4a3's50-boot
executor matrix does not qualify newer source. No new full matrix for unused
helpers. Self-review only, not independent audit.

Actual RPC list/admission/callback/commit, FID/DAC and fixed/provided-buffer
dispatch and readiness integration remain before AS3/4 and MM0-MM4. Canonical
source supersedes the installed draft; no historical installer may be rerun.
No Main/private async/clipboard activation or128MiB protection change.

### Astra isolated FID ownership helper, October9

Prepared an uninstalled kernel FID mapping helper under
`work/oct5-as-r9/astra-takeover/fid-draft`. Exact WALK/open/CLUNK claims retain
physical slot identity; source borrows are counted separately from root lifetime
pins. CLUNK blocks new borrows and drains old ones; abort cleanup and child reap
retain the exact parent until local users leave. FID-F1 self-review repaired an
incorrect proposed aggregate-pin check for root open. The64-byte node still fits
the existing4096-byte charged table; actual ARM64 objects add no libc dependency.

Sequential actual-C89checks and24mutants pass under LLVM22 ASan/UBSan; existing
node fixture and11mutants pass. Source hashes and raw results verified. Early
masked mutation witnesses are preserved and repaired. Read fid-draft/state.json
and self-review.txt. This is helper-only evidence, not native RPC/authority or
concurrent-engine qualification; canonical kernel unchanged. Actual RPC/root
binding/retirement/DAC/buffer/readiness integration remains next, then AS3/4 and
MM0-MM4. Mac released. No Main/private activation or128MiB protection change.

### Astra FID root and teardown integration, October10

Installed the verified FID mapping/claim/source-borrow helper. CONNECT publishes
its actual attached root mapping with READY/terminal under Ll after abort checks.
Final local destruction retires/reaps derived FIDs before their parent, refusing
remaining claims or borrows; scope reap also checks the new ownership fields.
The expanded normal/bulk native storage fixture holds an actual root metadata
borrow and unused child across close while charged operation storage retains
the engine entry. Close parks until physical ownership returns, then cleans up.

Real kernel link, CONNECT11schedules/14mutants, final metadata11cases/7mutants
pass. Native `fid-integration/native-20261009T221811Z` passes1854/1854 plus D7
overlap/20cycles/banner, CPU1; eight source and six artifact hashes stay stable,
raw evidence rechecked. Derived mapped cleanup is host-simulated, not a native
WALK/CLUNK claim. Read `fid-integration/self-review.txt`. This bounded integration
does not inherit the old full Mac qualification or implement general RPCs.
Canonical sources supersede fid-draft. Mac released. Next actual RPC admission,
callback/commit/readiness and payload/DAC consumers, then AS3/4 and MM0-MM4.
No Main/private user async/clipboard activation or128MiB protection change.

### Astra isolated RPC ownership transitions, October10

Prepared an uninstalled bounded per-scope operation list and queued/inflight/
settling/terminal/retired transitions in `astra-takeover/rpc-draft`. One exact
request/physical owner and continuous borrow retain queued, callback and physical
settlement work. Callback only captures; success rechecks abort/deadline. CQ
backpressure keeps terminal owners findable, and delivery/unlink cannot drop
physical ownership. RPC-R1 self-review added scope-abort enforcement for malformed
or duplicate capture rather than treating it as an isolated operation failure.

Actual-C50checks/21mutants pass with real request/node/slot/reply modules and
explicit host boundary doubles under ASan/UBSan. ARM64 real-header object passes:
640-byte operation,42288-byte protocol; no libc dependency. Raw source/binary
hashes rechecked. No actual client submission, authority, native runtime or
concurrency claim. Canonical kernel unchanged. Read rpc-draft/state.json,
self-review.txt and next-work.txt; wire/program/admission/readiness/physical-close
integration remains next, then AS3/4 and MM0-MM4. Mac released; no activation.

### Astra RPC wire adapter, October 10

The uninstalled `astra-takeover/wire-draft` now builds actual GETATTR, WALK,
LOPEN, READ, WRITE and CLUNK frames from retained kernel arguments. It enforces
negotiated message/output/iounit bounds and records the matching reply identity.
WIRE-R1 self-review distinguishes synchronous local EAGAIN (proved unsent) from
a remote Rlerror(EAGAIN), which must never authorize replay of a write.

156 checks and 14 targeted mutants pass ASan/UBSan with the actual submission
function, session, codec, transport and capture code; host lock/wake/death
boundaries and direct reply dispatch are explicitly limited. ARM64 object with
real headers passes without libc dependencies. Raw source/binary hashes checked;
failed harness runs preserved. This is uninstalled adapter evidence, not native
engine or authority qualification. Canonical kernel remains unchanged; no prior
full Mac gate is extended to this draft. Read wire-draft/self-review.txt and
next-work.txt. Actual DAC/payload admission, fair READY progress and coherent
close disposal remain next, then AS3/4 and MM0-MM4. Mac released. No activation.

### Astra staged WALK/open authority program, October 10

The uninstalled `astra-takeover/program-draft` composes GETATTR, ordinary DAC and
WALK/LOPEN. Component WALK checks directory type before search permission; LOPEN
rejects symlinks before access checks, preserving the ordinary single-hop rule.
Each program retains one admission-time identity value; retries cannot borrow a
worker's identity or recapture later grants. Negative remote EAGAIN remains a
terminal outcome. Program outcomes still require the owner's abort/deadline,
physical-release and final publication gates.

The actual permission implementation and wire/submission path pass 4,608 DAC
combinations, 18,711 total assertions including setup/wire checks, and 15 targeted
mutants under ASan/UBSan. ARM64 compilation passes without libc dependency.
Raw source/binary hashes rechecked; a masked success-guard witness was repaired,
and earlier failures retained. This is bounded host evidence with explicit
lock/wake/death boundaries, not actual creator admission or native engine proof.
Canonical kernel remains unchanged. Read program-draft/self-review.txt and
next-work.txt for coherent owner/FID/buffer/READY/close integration next, before
AS3/4 and MM0-MM4. Mac released; no activation or128MiB protection change.

### Astra combined RPC and permission owner, October 10

The uninstalled `astra-takeover/owner-program-draft` now connects the charged
operation, actual submission/capture and staged DAC program. One program.wire
holds the expectation and retained payload used by both builder and callback.
The constructor retains its fresh creator/image identity value. OP-R1 self-review
adds once-per-attempt submission/advance guards and requires a completed program
before positive terminal publication; a stat reply cannot bypass the effect.
Validation refusal without callback is captured locally once, while accepted
negative callbacks remain single completions. Requeue checks the absolute deadline.

ASan/UBSan:160 integrated checks and17 targeted mutants, plus constructor clean
fault/cleanup cases and14 mutants. Interleaved operations complete in reverse
order into distinct retained buffers. Actual ARM64 objects pass without libc:
owner776B fits its charged page; protocol42288B retains prior allocation size.
Raw28input hashes/18 RPC binary pairs,15 constructor pairs and3 ARM objects were
rechecked. Host snapshot/allocator/engine and lock/wake/death boundaries remain
explicit; no actual native admission, progress-abort or concurrent-close claim.
Failed harness evidence retained; single-agent self-review only.

Canonical kernel remains97d495fcb. Use this combined owner rather than the old
rpc-draft copies, with exact program-draft/wire-draft dependencies. Actual full
FID/destination and fixed/provided payload admission, READY fairness/readiness and
physical close disposal must be completed coherently before queue installation.
Then native/source-matched gates, AS3/4 and MM0-MM4. Mac released, no running gate;
Stratum61dde37 and Astra10protected/pending hashes unchanged. No activation.

### Astra FID admission transactions, October 10

The uninstalled `astra-takeover/admission-draft` resolves exact source scopes,
retains READ/WRITE access borrows, and claims WALK destinations or exclusive
opens. ADMIT-R1 self-review moves the pure queue/request check before irreversible
CLUNK retirement, retaining Ll through claim and enqueue. CLUNK submission waits
for earlier source borrowers. Final publication, hold return and terminal commit
share a fresh abort/deadline check under Ll. CLUNK error cleanup requires both
scope-abort latch and externally established callback quiescence.

257 cumulative sanitizer checks (160 prior owner checks plus97 new admission/setup
assertions) and26 targeted mutants pass; constructor fault/cleanup controls add16
mutants, including refusal to free while FID holds remain. Actual ARM64 owner816B
still fits its charged page, protocol42288B unchanged, no libc references. Raw31
input hashes/27 RPC pairs,17 constructor pairs and4 ARM objects rechecked. A masked
quiescence witness was corrected; failed evidence retained. Single-agent self-review.

These are actual FID/ledger transactions with explicit synthetic program success
in the new cases, not native remote-FID/driver/close proof. Canonical kernel remains
97d495fcb. Next immutable SQE frontend and canonical fixed/provided buffers, actual
monotone wire-FID allocation, then coherent READY/physical close integration before
queue installation. Native matched gates, AS3/4 and MM0-MM4 follow. No activation.

### Astra fixed-buffer SQE preparation, October 10

The uninstalled `astra-takeover/fixed-io-draft` now prepares an immutable kernel
SQE with exact creator identity, charged owner, canonical fixed extent or copied
WALK component, and actual monotone wire-FID allocation. Accepted backing survives
registered-table replacement; zero-length I/O validates index/offset without a
payload hold. Fresh publication transfers ownership atomically with a separately
retained wake root, avoiding operation access after the worker may consume it.
A physical-drop barrier prevents terminal/free while an unlinked backing pin is
still being returned outside Ll; payload pointer and capacity clear afterward.

268 ASan/UBSan checks and17 targeted mutants pass. Real ARM64 owner864B still fits
its charged page, protocol42288B unchanged. Raw30 inputs/18 source-binary-log pairs
and5 ARM objects reverified. Masked creator-image/empty-index witnesses were
strengthened; failed evidence retained. Single-agent self-review only.

This is an internal immutable-copy API, not shared-ring dispatch or a native
queue. Host address translation, snapshot, allocator/ref/physical-drop boundaries
and manual admitted destruction remain explicit. Canonical kernel remains97d495.
Next provided-buffer selection/leases/MORE/empty waits, then coherent READY and
physical close cleanup before queue installation; native matched gates, AS3/4
and MM0-MM4 follow. Mac released, no jobs,9176MiB before documentation; Stratum
clean61dde37 and Astra10 pending/protected hashes intact. No activation.

### Astra provided-buffer operation ownership, October10

The uninstalled `astra-takeover/provided-io-draft` now retains pool streams through
operation bookkeeping, selects canonical BUSY members before reads, waits locally
on empty pools, and preserves exact leases across source retirement. Single-shot
positive payload and terminal share one CQE/receipt publication. Multishot has one
pending payload per request; cancellation cannot overtake it, and rearm requires
fresh abort/deadline checks plus an explicit delivered-MORE proof. Self-review
added that proof after finding that released BUSY alone could permit silent loss
of an uncommitted positive reply. No CQ acknowledgement automatically returns a
lease; EOF/errors release only unpublished BUSY members.

554 cumulative sanitizer checks (268 prior fixed plus286 pool/setup/cleanup)
and18 targeted mutants pass; ARM64 owner920B fits its charged page. Raw33inputs/
19source-binary-log pairs and6ARM objects reverified. Synthetic parsed replies
feed actual capture/program/ledger/CQ code; physical registration, host refs and
manual admitted destruction are explicit fixture boundaries. This is not native
transport, driver, concurrent-close or shared-ring-dispatch qualification.

PROVIDED-R3 remains a concrete close integration dependency: global request detach
requires zero borrows, but pending multishot keeps a borrow and cannot be cancelled
before its committed success. Whole-ring close needs its own actual consumer/
writer-quiescence proof and pending-disposal edge before global detach. Scope
abort must keep pending/leased payloads intact. Complete READY/physical close and
fair empty-pool/return wake integration coherently before installing the queue.
Then native matched gates, AS3/4 and MM0-MM4. Canonical kernel remains97d495fcb.
Mac released, no native job,8940MiB before docs; clean Stratum61dde37 and Astra10
pending/protected hashes intact. Single-agent self-review; no activation.

### Astra operation retirement and pool destruction draft, October10

The uninstalled `astra-takeover/retirement-draft` now supplies actual admitted
operation destruction, client-abort quiescence and per-owner whole-ring disposal.
RETIRE-R1 reproduced an actual client abort defect: progress.sending was cleared
but the RPC sending flag survived, preventing settlement even after callbacks.
The draft clears that hold after transport abort and before callbacks, including
an early-replied RPC no longer in the inflight table. Legacy senders are unchanged.

RETIRE-R2 separates terminal commit from delivery: negative commit releases local
FID/request borrows while a committed MORE stays intact. Operation-aware delivery
keeps MORE before its negative terminal. PROVIDED-R3's draft whole-close disposal
now precedes global detach, without consuming another constructor reservation.
Physical cleanup orders backing return, free, refund, request owner-drop/reap,
then engine/Loom release. Delivered leases survive operation destruction. Only
final zero-reference ring destruction with no writers/owners validates and reaps
the entire pool bank, then physically drops every saved backing pin.

795 cumulative ASan/UBSan checks and15 targeted mutants pass;39 current inputs,
16 source/binary/log pairs and10 real ARM64 objects reverified. Owner920B and
protocol42288B unchanged, no libc dependency. Raw baseline failure and harness
failures retained. Host native teardown/ref/engine/allocation/physical-drop
boundaries, synthetic successful replies and simulated final references remain
explicit. Actual READY/executor routing and native close/exec qualification are
still owed. These fixes remain uninstalled; canonical kernel remains97d495fcb.
Read retirement-draft/state.json, self-review.txt and next-work.txt. Review is
single-agent, not an independent audit. Prior full Mac gate qualifies only its
recorded f719ee4a3 source. Mac released, no native jobs,8843MiB above8192floor;
Stratum clean61dde37 and Astra10 pending/protected hashes unchanged. Next coherent
READY/RETURN/frontend integration and matched native gates, then AS3/4 and MM0-MM4.
No Main/private async/clipboard activation or128MiB protection change.

### Astra bounded READY selector, October10

Added uninstalled `ready-draft`, using the verified retirement ownership APIs.
It samples no operation cursors while another driver/teardown user owns them,
exports value-only wait interest, then rescans and atomically claims driving plus
round-robin list rotation before returning a selected owner. Empty pool/full tag
or TX shortages park; deadlines remain visible through full CQ. Local retirement
does not hide pending delivery/storage owners. READY-R1 self-review repaired
first-runnable selection so a later expired owner preempts earlier submission.

54 sanitizer checks/12mutants,17input hashes/13 raw pairs and one real ARM64 object
pass. Fixtures synthesize ledger/readiness states; actual action execution,
engine/CONNECT/close routing, RETURN wakes and native scheduling are still owed.
This is a selector prerequisite, not installed dispatch. IOUNIT-R1 records the
remaining negotiated-iounit metadata/admission wiring before actual dispatch.
Canonical97d495 unchanged; Mac released,9163MiB, Stratum clean61dde37 and Astra10
hashes intact. Continue ready-draft/next-work; retirement-draft remains required.
Single-agent self-review only; no activation. Native matched gates, AS3/4 and
MM0-MM4 remain ahead.

### Astra bounded READY action executor, October10

Uninstalled `action-draft` now executes actual submission, reply advance,
settlement, commit, cancellation, delivery, rearm and physical destruction under
an exclusive driver claim. A separate caller-owned entry/Loom root must survive
operation free. Fresh deadline checks follow physical drops and precede new
attempts/commits. Failed requeue settles its consumed callback before scope abort;
remote AGAIN stays terminal and failed CLUNK requires actual scope quiescence.
PENDING MORE survives negative commit and precedes its terminal delivery.

ACTION-R1 reproduced the older selector submitting despite full CQ, contrary to
lifecycle section7. Corrected selector requires CQ room for new wire submission,
while abort/deadline and already-inflight IO stay available. Preserved failing
baseline and exact guard-removal mutant. This selector supersedes ready-draft's
working source; action-draft still depends on retirement/program/wire drafts.

1633 cumulative ASan/UBSan checks and15 exact mutants pass;45 input hashes,
16 raw source/binary/log pairs and2 real ARM64 objects reverified. Actual native
progress/demux, successful replies, engine pins/refs and allocation/backing-drop
boundaries remain controlled; this is not native engine/close qualification.
No canonical kernel change (still97d495), activation or128MiB removal. Single-agent
self-review only. Mac released,8876MiB; Stratum clean61dde37 and Astra10hashes
preserved. Continue IOUNIT-R1 negotiated metadata, then coherent entry-pinned
engine/CONNECT/close routing, RETURN/frontend wakes and native matched gates;
AS3/4 and MM0-MM4 follow. See action-draft/state/self-review/next-work.

### Astra negotiated I/O metadata, October10

Uninstalled `iounit-draft` resolves IOUNIT-R1 after actual action checkpoint
473a54bf. A validated Rlopen limit is now retained in unused internal node space
and published atomically with open access under Ll. Exact READ/WRITE admission
copies it into the immutable wire program, including provided and rearmed reads.
Zero retains no additional limit; nonzero cannot widen the negotiated frame size.
Expiry publishes neither access nor limit, and reap/reuse clears the metadata.
Actual baseline lost-limit failure is preserved beside the corrected sources.

2316 cumulative ASan/UBSan checks and11 exact mutants pass;47inputs/12raw
source-binary-log pairs/4ARM objects reverified. Node64B/table4096B/owner920B
unchanged. Captured replies, native progress and engine/ref/allocation boundaries
remain controlled; no native negotiation or integrated scheduling claim. Canonical
kernel remains97d495. Macfree/nojobs/8838MiB; Stratum clean61dde37 and Astra10hashes
intact. Continue coherent independent-entry-pinned engine/CONNECT/close routing,
immutable shared frontend and RETURN/CQ wakes, then actual-source/native matched
gates, AS3/4 and MM0-MM4. Read iounit-draft/next-work for exact working overlays.
Single-agent self-review only; no Main/private activation or128MiB removal.


### Astra entry-pinned engine and close drain, October10

Uninstalled `drain-draft` now routes active CONNECT through handshake handling
and completed CONNECT through general operation readiness. Each READY/close visit
retains a separate entry pin and Loom root through consuming operation destruction.
Local retirement waits for actual RPC quiescence and local borrows; retained MORE
and terminal owners remain visitable. Whole close disposes them before global
request detach and zero-pin protocol collection. Native wait registrations retain
entry pins until unhook, so final collection parks until their return.

DRAIN-R1 reproduced stale readiness permitting IO after a request deadline; a
fresh exclusive-driver preflight now checks time and abort/close before wire IO.
DRAIN-R2 fails closed on unexpected internal action refusal. DRAIN-R3 parks CLUNK
cancellation/discard while older FID borrowers remain. A surviving mutant exposed
a weak cancellation test; its corrected witness checks actual wait-arm readiness.
Failed runs remain preserved. Completed CONNECT deadlines and separate CONNECT
completion quanta are covered alongside retained-wait close and pending MORE.

2885 cumulative ASan/UBSan assertions and12 exact fault variants pass;53inputs,
13raw source/binary/log pairs and6ARM64 objects reverified. Existing lifecycle
models pass2clean/14counterexamples; unchanged models do not prove new C routing.
Prebound engine setup, native IO/poll/teardown, parsed successes, refs/allocation/
notification and final protocol destruction remain controlled host boundaries.
No native scheduling/full-source gate or installed dispatch claim. Canonical
kernel stays97d495. Mac released/no jobs,8769MiB above8192floor; clean Stratum61dde37
and Astra6pending/4protected hashes intact. Single-agent self-review only.
Continue actual immutable shared frontend and pool RETURN/CQ wakes, coherent
installation and matched native/full Mac gates, then AS3/4 and MM0-MM4. No Main,
private async/clipboard activation or128MiB change. See drain-draft/next-work.


### Astra exact payload return and registered wake, October10

Uninstalled `return-draft` adds the creator-authorized copied-record RETURN
control. Exact image/creator authority, negotiated receipt feature and shape
validation precede a locked live pool-slot/LEASED receipt transition. Successful
return wakes the existing engine predicate after Ll; it needs no SQ/CQ room,
allocation or source protocol pointer. A parked empty-pool reader wakes even
with full CQ, then respects backpressure before claiming a fresh lease. Pending,
stale/double and old-incarnation returns cannot release a newer payload.
Leases remain returnable after source protocol collection, and pool-slot reuse
preserves the old receipt's invalidity.

3125 cumulative ASan/UBSan assertions (2885 prior drain regressions) and10 exact
fault variants pass;55inputs/11rawpairs/2ARM64 control-layout objects reverified.
Initial fixture wrongly tried pool registration after whole-ring close; correct
scope-only retirement/reuse test now passes, with failure retained. Native
wake/poll/IO, parsed replies, allocation/backing/ref and final protocol destruction
remain controlled boundaries. No syscall/shared-SQE/native scheduling claim.
Single-agent self-review only. Canonical97d495 unchanged; Macreleased/nojobs,
8736MiB above8192floor, clean61dde37 and Astra10hashes intact. Continue immutable
shared-SQE/creator frontend and CQ kick, coherent install and matched native/full
Mac gates, then AS3/4 and MM0-MM4. No activation/128MiB protection change.
See return-draft/state,self-review,next-work; it overrides only control/header
before the exact drain-draft overlay stack.


### Astra immutable shared frontend, October 10

Uninstalled frontend-draft copies accepted shared SQEs once under a serialized
claim, bounds budget/indices by private ring geometry, and invokes actual fixed
or provided preparation/publication outside Ll with fresh authority/deadline
checks. A precharged scalar refusal record preserves exactly one negative CQE
when capacity disappears after consumption; pending refusal blocks another claim.
Zero-budget creator ENTER kicks registered CQ work. Close waits for preparation
and physical rollback before discarding settled refusals and draining owners.

FRONT-R1 reproduced priority refusal starvation; the fixed engine gives the
refusal one turn among64scopes and normalizes close cursor back to64. Actual host
regressions pass3674 cumulative sanitizer assertions/13exact mutants,59inputs/
14rawpairs. Three real ARM64 objects pass;3664Bengine fits charged4096B page.
Existing lifecycle models2clean/14counterexamples pass, without modelling this
new refusal record. Failed fixture/build/model evidence is retained as applicable.
Single-agent self-review only. See frontend-draft/state,findings,self-review.

CONNECT successful actual constructor/adoption is still owed: this fixture tests
scalar routing into a controlled constructor refusal. Native IO/poll/wake/ref/
backing/allocation/final destruction remain controlled boundaries. No installed
syscall or native/full Mac qualification. Canonical kernel97d495 unchanged;
prior50/50 qualifies onlyf719ee4a. Macreleased/nojobs,8684MiB above8192floor;
Stratum clean61dde37 and Astra6pending/4protected hashes verified. Continue
frontend-draft/next-work combined CONNECT, coherent installation and matched
native/full gates, then AS3/4 and MM0-MM4. No Main/private async/clipboard activation
or128MiB protection removal.


### Astra frontend CONNECT composition, October10

frontend-connect now joins actual request/scope reservation, CONNECT preparation,
publication, rollback and engine adoption to the immutable frontend.3948 cumulative
sanitizer checks/7exact mutants pass, with copied target/correlation, nested claim,
success, constructor refusal, fresh expiry, close and native publication refusal.
Adopted failure retains its one engine terminal; pre-adoption rollback preserves
obligations through physical destruction. Production drafts unchanged. Native
protocol allocation/init, publication effects and handshake replies remain checked
host boundaries. Source/log/executable hashes reverified; self-review is single-agent.
Failed fixture expectations and mutant compile diagnostics retained, not counted
as successful fault detections. Continue coherent current-overlay install and
matched native/full Mac gates, thenAS3/4,MM0-MM4. Prior50/50 onlyf719ee4a. No Main,
private async/clipboard activation or128MiB removal. Macreleased; no active job.


### Astra coherent installed async integration, October10

The current38 reviewed source/header overlays are now INSTALLED in canonical
kernel. This supersedes earlier uninstalled pickup directions: never reinstall
historical draft copies. INTEGRATE-R1 corrected operation's isolated relative
slub include; full ARM64 link and symbol-table regeneration pass. Read
astra-takeover/async-integration/state,installed-overlay,self-review,findings.

New native frontend probe uses the exact creator, real target/protocol constructor
and engine adoption for normal/bulk connections, actual handshake/root mapping,
copied correlation, malformed refusal and CQ-acknowledgement wake, followed by
actual charged storage/close ownership. Native1855/1855,D7overlap,20login cycles
pass with41source/6artifact hashes stable; minimum8617MiB. Existing host regression
3948assertions/7CONNECTmutants and3674/13frontendmutants remain bounded evidence.
Native general WALK/LOPEN/READ/WRITE/provided/MORE traffic still needs qualification.
Self-review is single-agent. Resource procedure error and original include failure
are recorded honestly. Macreleased/nojobs; Stratumclean61dde37/Astra10hashes intact.

Next full current-source Mac matrix without source edits, raw verification, then
native general-operation/provided tests and AS3/4 before MM0-MM4. Prior50/50 only
qualifiesf719ee4a. Public private ENTER remains disabled. No Main/private async/
clipboard activation or128MiB removal. Preserve all matched evidence.


### Astra disk-floor recovery and pinned image matrix, October10

Whole-image gate current-gate-20261010T011053Z was stopped by the independent
disk guard at8247MiB (8192floor plus64MiB stop margin), during pool rebaking.
No matrix boots ran and no gate pass is claimed. Owned processes exited and Mac
was released. Failed run, keys/image hashes and recovery metadata are preserved.
Exact ramfs/pool snapshot/system key clones were restored from immutable inputs,
verified against the successful1855 native run; recovery returned8722MiB free.

Next async-integration/pinned-matrix.py rebuilds default and UBSan kernels from
current source only, derives all five rows from ci-smp-gate, runs the actual
10-boot multiboot harness per row, captures8matched inputs and verifies100raw
logs,1855tests,D7/20cycles,source/protected hashes and the continuous disk guard.
Report this as the complete matrix stage with pinned image inputs, not as a
successful whole-image ci-smp-gate rebuild. Read the newer
astra-takeover/async-integration-matrix-flight.json when present; do not duplicate
its work or modify tracked files while active. Prior native/host evidence remains
bounded; native general operations/provided traffic,AS3/4 and MM0-MM4 follow.
No Main/private async/clipboard activation or128MiB removal; single-agent review.


### Astra installed async matrix verified, October10 02:02 UTC

The five-row pinned-image Mac matrix passed50/50 at exact clean source
98dd4b6183c002fa2ead543b6fb350da604cdec0: default SMP1/4/8 and UBSan SMP4/8,
ten boots each. Raw verification rechecked100serial/harness logs,1855/1855
tests per boot,20private ownership/frontend probes,D7overlap and20login cycles,
eight captured/current artifact hashes,clean61dde37 and Astra's10preserved files.
Minimum free space8510MiB stayed above8192floor. Run: work/oct5-as-r9/
pinned-matrix-20261010T011750Z. Runner exited0 and released Mac.

This is kernel matrix qualification with pinned native-qualified image inputs;
it does not convert the earlier stopped whole-image rebake into a pass.
Single-agent self-review of source freeze, input copies and verifier assertions
found no new discrepancy. Native general WALK/LOPEN/READ/WRITE/CLUNK and provided/
MORE traffic remain next, then AS3/4 and MM0-MM4. No Main/private async/clipboard
activation or128MiB protection removal. Historical installers must not be rerun.


### Astra native general operations and provided payloads, October 10

Added `loom.private_native_operations` in both normal/bulk classes. A controlled
peer now reads emitted frames before replying through real SrvConn queues. The
installed frontend/engine performs CONNECT, search+WALK, DAC+LOPEN, registered
READ/WRITE and CLUNK. Accepted SQE correlation and WALK bytes remain immutable;
read bytes and emitted write bytes match, negotiated iounit persists, and physical
request/fixed ownership returns before unregister.

Provided READ exercises a one-member pool, paired receipt, CQ consumption without
payload return, duplicate return rejection, empty-pool parking, fresh MORE leases,
RETURN while CQ is full, resumed dispatch after drain, EOF without an empty lease,
and final pool reap. The fixture drives bounded engine visits; general operation
worker scheduling and hostile/stalled-peer coverage remain AS-3 work. The peer is
a deterministic wire responder, not a filesystem implementation.

`native-operations/provided-20261010T0210Z` passes1856/1856,D7overlap/20cycles with
41source and6artifact hashes stable; minimum8283MiB. Evidence is under
work/oct5-as-r9/astra-takeover/native-operations. Failed freestanding aggregate
initializer build and initial wrong CQ flag expectation are preserved with fixes.
No production-path defect was established by those fixture failures. Self-review
is single-agent. Mac released; no public private ENTER/clipboard activation or
128MiB change. Earlier50/50 remains source98dd4b618 only; new fixture results are
bounded native evidence. Continue AS-3 C/Rust ownership and adversarial native
schedules, then AS-4 and MM0-MM4.


### Astra worker fixture verification in flight, October 10

Native general/provided sequence has been changed to use the actual private
worker: the fixture only yields, reads peer frames and acknowledges CQ via the
creator frontend; no explicit engine visit drives this probe. Initial native
worker run passes1856/1856. Self-review then replaced the concurrent test wake
epoch read with a locked snapshot and CQ-tail read with acquire publication.
That newer fixture requires verification. Read native-worker-flight.json before
acting: five one-boot default SMP1/4/8 and UBSan SMP4/8 smoke rows are next.
They are bounded smoke coverage, not a replacement for the50-boot gate.
Do not change tracked source or duplicate jobs while it is active. No public
private async/clipboard activation or128MiB removal. After verification preserve
raw evidence, checkpoint and continue AS-3 hostile peer and C/Rust owned-buffer
APIs. Raw Rust register_buffers is already unsafe; inspect current clients before
repeating historical repair directions. AS-4 and MM0-MM4 remain afterward.


### Astra worker smoke disk stop and recovery, October 10

smoke-20261010T021509Z stopped at8255MiB (64MiB guard margin above8192floor),
before its first boot completed. No boot pass. Raw serial/harness/source/binary
hashes preserved in interruption.json; no VM or test jobs remained. Concurrent
wrapper shutdown had prevented the diagnostic result from being saved. The
diagnostic now catches termination, joins its own child group in finally and
records both interruption and guard state.

Cleared only Go's rebuildable compilation cache with go clean -cache (799MiB);
free space recovered to9060MiB. Sources, saved binaries and evidence remain.
Restart the same five one-boot smoke rows from native-worker/run-smoke.py; consult
native-worker-flight.json for the new run and keep source frozen. Qualification
remains bounded; no50boot claim for this newer fixture and no activation.


### Astra actual worker operation smoke verified, October 10

The native general/provided probe now runs entirely through the actual private
worker: no explicit engine visits drive it. CQ consumption uses acquire loads
and zero-submit creator kicks; physical terminal cleanup is observed under Ll.
Wake epoch and native channel occupancy observations take their owning locks.
Whole-close joins the worker before peer/mapping cleanup.

Five source-matched one-boot rows pass: default SMP1/4/8 and UBSan SMP4/8, each
1856/1856,D7overlap and20login cycles. Run native-worker/smoke-20261010T021745Z
under work/oct5-as-r9/astra-takeover. Reverified10rawlogs,41installed source hashes,
six inputs per run with cloned kernels,Stratum clean61dde37 and Astra10preserved
files. Minimum9056MiB. These smoke rows do not replace the50boot qualification
of98dd4b618. The controlled peer still leaves hostile/stalled schedules and safe
client APIs pending. Self-review is single-agent; no activation/128MiB change.

Earlier smoke021509Z stopped at8255MiB before boot completion, preserving raw
interruption evidence. Diagnostic signal handling now joins its child and saves
interrupted results. Clearing only799MiB of rebuildable Go cache restored space;
no source, saved binary or evidence was removed. Mac released after successful
smoke. Continue AS-3 native faults and owned C/Rust clients, then AS-4/MM0-MM4.


### Astra native READ faults verified, October 10

`loom.private_native_faults` adds six worker-driven READ schedules in both normal
and bulk classes after real CONNECT/WALK/LOPEN: silent peer, partial header/body,
impossible frame length, wrong reply type and full-CQ cancellation. The retained
peer endpoint and observation pin survive until local retirement. Negative CQE
identity, fixed-owner release, unregister and unchanged failed-read payload are
checked. Full CQ retains the cancellation terminal until creator acknowledgement.

Five single-boot rows pass1857/1857,D7overlap/20cycles: default1/4/8,UBSan4/8.
Run astra-takeover/native-faults/smoke-20261010T023045Z, sourcecc644dc6a plus
recorded diff62dc1a57. Ten raw logs,41sources,six inputs/row,kernel clones and
Astra10protected/pending hashes reverified; Stratum clean61dde37; minimum9221MiB.
This is bounded smoke evidence, not50boot or completed AS3 qualification.
Single-agent review records that fragment consumption before cancellation is
scheduler-dependent; next change will observe actual transport drain. Prepared
followup-write-deadline.patch is UNINSTALLED and also adds emitted WRITE abort
and READ deadline cases. No production kernel change or activation/128MiB change.
Mac released. Continue broader AS3 faults and owned clients, thenAS4/MM0-MM4.


### Astra emitted WRITE and deadline faults verified, October 10

Expanded native fault fixture to eight schedules per normal/bulk class. Canceled
WRITE is observed after its bytes reach the peer without acknowledgement; its
negative terminal and retained BYTES_SENT diagnostic do not promise remote
rollback. Silent READ expires by absolute500ms deadline and locally retires.
Partial header/body cancellation now waits for actual s2c channel drain under
the channel lock, establishing consumption before abort instead of yield counts.

Five smoke rows pass1857/1857,D7overlap/20cycles at bdba6bc5e plus diff504cd6018,
run native-faults-followup/smoke-20261010T023726Z. Reverified10rawlogs,41sources,
six inputs/row,kernel clones,Stratum clean61dde37 and Astra10preserved hashes.
Minimum9252MiB; Mac released. No50boot or completeAS3 claim. Single-agent review;
no production change, private activation or128MiB removal. Next reviewed but
UNINSTALLED native-reconnect/reconnect.patch adds1000 canceled-before-accept
attempts alternating classes with complete per-attempt ownership/accounting checks.
Broader AS3 stage/close/exec/exit/independent-media and owned clients,AS4/MM0-MM4 remain.


### Astra1000 failed native reconnects verified, October 10

One private worker/ring now survives1000failed CONNECTs before server acceptance,
alternating500normal/500bulk. Each exact canceled terminal precedes scope reap,
local parser/request/page refund and stale-identity refusal. A torn backlog peer
retains count+1 and credits+1or4; late acceptance then last-unref restores native
count/credits exactly. Retirement does not pretend peer-held storage is free.

Initial smoke024406Z failed1857/1858 because the fixture wrongly expected native
endpoint/backlog refund before late accept. RECONNECT-FIXTURE-R1 is preserved
with raw source/binary/logs; corrected assertions follow approved design section5.
No production code changed. Corrected smoke024610Z passes five rows1858/1858,
D7overlap/20cycles, default1/4/8+UBSan4/8. Source4fe965d62+diff408aed6b;
10rawlogs/41sources/sixinputs per row/kernel clones/Astra10hashes/clean61dde37
reverified, minimum9209MiB. Self-review is single-agent; no50boot or AS3 completion.

Next reviewed UNINSTALLED native-reconnect-retained/retained.patch adds a second
1000attempts with no server acceptance:32torn endpoints hold80credits and968
backlog refusals must not grow retained storage. Verify after checkpoint. Broader
AS3/owned clients,AS4/MM0-MM4 remain; no activation/Main/128MiB change. Mac released.


### Astra retained-backlog stress verified, October 10

The actual private worker now completes two 1000-attempt reconnect loops per
boot. The second accepts no peer during retries: 16 normal and 16 bulk torn
endpoints retain exactly 80 credits, while 968 backlog refusals leave retained
storage bounded. Each local scope retires/reaps; final late acceptance and last
unref restore the original native endpoint/credit counts. No production change.

Run native-reconnect-retained/smoke-20261010T025237Z passed five one-boot rows
(default 1/4/8, UBSan 4/8), 1858/1858 tests, D7 overlap and 20 login cycles each.
Source 8f31d58b9 plus diff ec849c4ad3; 10 raw logs, 41 source hashes, six inputs
per row, cloned kernels, clean external Stratum 61dde37 and Astra's ten protected
hashes reverified before documentation changes. Minimum free space 9165 MiB.
This is bounded smoke evidence, not a new 50-boot qualification or AS3 completion.

Single-agent self-review recorded. Next native-stages/stages.patch is reviewed,
UNINSTALLED and unverified: cancellation after each of six complete setup
requests, in both connection classes. Broader close/exec/exit, independent media,
owned clients, AS4 and MM0-MM4 remain. No Main/private activation/128 MiB change.


### Astra native setup-stage cancellation verified, October 10

Actual worker cancellation now passes at six complete emitted requests: VERSION,
ATTACH, pre-WALK GETATTR, WALK, pre-LOPEN GETATTR and LOPEN, in normal/bulk
classes. The peer sends no reply for the selected request; local retirement,
exact negative terminal (no successful descriptor), physical request/fixed-owner
release and unregister complete while retained peer/observation references remain.

Five one-boot rows default1/4/8 and UBSan4/8 passed1859/1859 plus D7overlap and
20login cycles each in native-stages/smoke-20261010T025901Z. Source51b95e440 plus
diff2e6e907363;10rawlogs,41installedsources,sixinputs per row and cloned kernels
reverified before docs changed. Clean61dde37 and Astra's10hashes preserved;
minimum9169MiB. Single-agent self-review; no production change or full50boot claim.

Next UNINSTALLED native-stage-close/stage-close.patch repeats those six requests
with final handle close, forced private exec cleanup and exit handle-table cleanup
(36schedules), with actual worker overlap, held references and no CQ/peer progress.
It also adds a common explicit final image-page/private-ring refund assertion.
These are production cleanup boundary witnesses; whole userspace exec/exit races,
independent media/owned clients, remaining AS3,AS4/MM0-MM4 still remain.
No Main/private activation or128MiB protection removal.


### Astra native close-boundary schedules verified, October 10

Six emitted setup requests x normal/bulk x final handle close, forced private
exec close (CLOEXEC false), and exit handle-table destruction:36 schedules now
pass with actual worker overlap. Retained observation pins force PARKED after
local transport retirement; returning them permits DONE, worker join and empty
physical request/fixed storage without CQ consumption or peer replies. The
common protocol fixture now explicitly verifies original image page_count and
zero private_rings after final retirement, before process destruction.

native-stage-close/smoke-20261010T030607Z passed five one-boot default1/4/8 and
UBSan4/8 rows,1860/1860tests,D7overlap/20cycles each. Sourcefc76769b1 plus
diffc9e5b59d77;10rawlogs/41sources/sixinputs per row/cloned kernels reverified,
clean external61dde37 and Astra10hashes preserved. Minimum9151MiB. This tests
production cleanup boundaries, not whole userspace exec/exit syscall races.
Single-agent review; no production changes, full50boot or AS3completion claim.

Next reviewed UNINSTALLED native-isolation/isolation.patch checks one byte stream
while a second scope stalls, then cancels only the latter and continues original
WRITE/provided/MORE/EOF/CLUNK with torn peer retained. Controlled native isolation
is not real media throughput. Owned clients, other AS3 integration,AS4/MM0-MM4
remain. No Main/private activation or128MiB protection removal.


### Astra same-worker scope isolation verified, October 10

One scope completes byte-checked READ while a second stalls at an observed
VERSION request. Canceling/reaping only the second preserves the first READY
transport and its WRITE/provided READ/MORE/EOF/CLUNK progress while the torn peer
remains held. Both classes pass on the same actual worker/ring. CQ4 uses existing
geometry for two CONNECT owners plus IO; full-CQ checks fill its actual capacity.

First run031230Z failed at link before any VM: a large fixture initializer emitted
unavailable memset (ISOLATION-FIXTURE-R1). Failed source/manifest/build log retained;
explicit existing byte-zero pattern fixes fixture only. Corrected run
native-isolation/smoke-20261010T031341Z passed five rows1861/1861,D7overlap/20cycles,
default1/4/8+UBSan4/8. Source5734fe538+difff3da283cc8;10rawlogs/41sources/sixinputs
per row/cloned kernels reverified before docs changed. Clean61dde37 and Astra10
hashes preserved;minimum9079MiB. Single-agent review, controlled native byte stream,
not real media throughput, whole userspace exec/exit or full50boot qualification.

Next reviewed UNINSTALLED native-midwrite/midwrite.patch injects c2s pressure
leaving5bytes, observes actual TWRITE prefix, then aborts without draining peer.
Private progress uses partial srvconn_io_nonblock, not legacy atomic frame send.
This is explicit transport pressure injection, not a valid peer transcript.
Owned C/Rust implementation source inspection is in owned-clients/; no safe-client
implementation/tests claimed. Remaining AS3/clients,AS4/MM0-MM4; no activation,
Main landing or128MiB protection removal.


### Astra partial-WRITE and client receipt ledger checkpoint, October 10

The actual private worker now emits exactly five TWRITE prefix bytes into an
explicitly pressure-filled channel, then aborts locally without peer drain.
Both classes verify exact negative terminal/BYTES_SENT, fixed-owner release and
unregister while peer/observation references remain. This is transport pressure
injection, not a valid peer transcript. Nine IO fault schedules per class.

native-midwrite/smoke-20261010T032012Z passed five one-boot rows1861/1861,
D7overlap/20cycles, default1/4/8+UBSan4/8. Source00f4d736d+diff106e399f2e;
10rawlogs,41sources,sixinputs/row,kernel clones and Astra10hashes reverified,
clean external61dde37; minimum9148MiB. Bounded smoke, not50boot or AS3completion.

Uninstalled C/Rust receipt-ledger foundations validate bounded geometry, exact
receipts and ring-local identity, stale/double/out-of-order returns and failed
RETURN retention. Earlier leases survive final/EOF. Self-review LEDGER-REVIEW-R1
added a distinct library owner identity to prevent matching tuples across rings
from returning foreign leases. Host ledger-20261010T032905Z passed Rust six cases
at opt0/opt2 and LLVM22 C ASan+UBSan; all12mutants compiled then failed assertions.
Eight source hashes and32command/rawlog records reverified. Initial Apple ASan
run032712Z stalled before main; HOST-ASAN-R1 sample shows reentrant ASan startup
spinlock. Failure evidence preserved; that C run is not a pass.

These are metadata foundations only, without owned backing, payload borrows,
paired CQ reap, futures, retirement or native safe-client qualification. Continue
owned C/Rust lifetimes then remaining AS3/AS4/MM0-MM4. Single-agent self-review;
no independent audit, Main landing, private activation or128MiB protection change.


### Astra paired completion-reader foundations, October 10

Uninstalled C/Rust paired readers now acquire CQ tail, copy the CQE and matching
receipt, then release head exactly once. Geometry validates aligned disjoint
header/CQE/receipt regions and bounded power-of-two entries; corrupt occupancy
or a second head writer refuses without acknowledgement. Returned records own
copies and survive slot reuse; this does not return or expose a payload.

owned-clients/paired-20261010T033551Z passed Rust four scenarios at opt0/opt2,
LLVM22 C ASan+UBSan, and100000 concurrent records per run across counter wrap.
All six receipt-index/double-ack/overlap mutants compiled then failed assertions.
Nine source hashes and20command/rawlogs reverified. Single-agent review records
unsafe mapping/producer contracts and C no-alias/exclusive-reader obligations;
concurrent stress is not a memory-model proof. Astra's10pending/protected hashes
and exact clean external61dde37 preserved. Mac released; disk remains above8GiB.

These helpers remain uninstalled and require owned mapping/backing, read-only
PayloadLease, future cancellation and bounded failed-return/client retirement.
No native safe-client qualification or complete AS3 claim. Continue those client
lifetimes before remaining AS3/AS4/MM0-MM4; no Main, activation or128MiB change.


### Astra owned payload lifetime cores, October 10

Uninstalled Rust/C cores consume backing and retain it independently of CQ and
client lifetimes. Read-only leases keep storage alive; explicit RETURN errors
preserve exact ownership/errno. Drop/abandon uses one pre-reserved pending cell
per member, with at most64retry attempts per progress call. Earlier A bytes survive
B return and final/EOF. Removing the retirement record requires client closing,
zero live leases and confirmed local retirement; controller destruction closes
once while existing leases retain backing. Native pins remain a backend contract.

Rust payload-20261010T034518Z passed six scenarios at opt0/opt2, four compile-fail
checks (borrowed return, mutable view, moved storage, legacy reap), valid-return
control and four runtime mutants. Initial034434Z negative fixture failed: wildcard
let _ did not access moved storage (PAYLOAD-FIXTURE-R1); evidence preserved, actual
field access now rejects E0382. C payload-c-20261010T034735Z passed six scenarios
under LLVM22 ASan+UBSan and optimized NDEBUG core objects plus four runtime mutants.
Five/seven source hashes and19/17raw command records respectively reverified.

Single-agent review derived lengths from owned backend rather than caller claims
(PAYLOAD-REVIEW-R1), and moved C state mutations outside assertions before release
controls (R2). Astra10hashes and clean external61dde37 preserved; disk9153MiB.
These are one-pool components with simulated backends, not native complete clients:
one shared full-ring controller, global64-member/cross-pool exclusion, syscall/
paired-CQ integration, fixed I/O ownership, futures and bounded executor retirement
still remain. Do not give each native pool independent whole-ring close authority.
Remaining AS3/AS4/MM0-MM4 pending; no Main, activation or128MiB protection change.


### Astra shared full-ring payload owners, October 10

Uninstalled C/Rust ring_payload cores replace independent one-pool retirement for
intended integration. One backend retains all backing and whole-ring authority;
pool drop closes only that pool. Other pools and live payloads keep their bytes.
All pools share64member admission and cross-pool overlap exclusions. Failed/busy
pool destroy retains slots/ranges/capacity; only zero permits reuse with a newer
full incarnation. Pending/lease cells total at most64, allocated at admission;
progress makes at most64RETURN and64destroy attempts without allocating.

Rust ring-payload-20261010T035754Z passes seven scenarios at opt0/opt2, four
compile-fail contracts plus positive control, and six runtime mutants. C
ring-payload-c-20261010T035801Z passes seven scenarios under LLVM22 ASan+UBSan
and optimized NDEBUG core plus six runtime mutants. Five/seven source hashes
and23/21raw command records respectively reverified. Earlier six-case runs
035413Z/035628Z retained and verified before adding explicit-return/EOF coverage.
Astra10hashes and clean external61dde37 preserved; disk9131MiB, Mac released.

Single-agent review records that separate pools need not mean independent scopes.
Native adapter still must pre-reserve local metadata before kernel registration,
retain failed commit cleanup, authenticate paired CQ/request identities, and bind
owned mapping/storage, fixed I/O, futures and executor retirement. Current unsafe
attach accepts only adapter-proven native pool identities; simulated backend tests
do not qualify that missing boundary. Remaining AS3/AS4/MM0-MM4 pending; no Main,
private activation,128MiB change or independent audit claim.


### Astra pool registration transactions, October 10

Uninstalled C/Rust shared-ring cores now prepare all ownership metadata and
precharge the global64-member bound before registration. Successful publication
binds the actual full pool identity without allocation. Confirmed negative+zero
output rollback refunds admission and preserves errno; uncertain, malformed,
stale or colliding results retain a quarantined reservation and close the whole
owner while existing leases keep their bytes. No destroy uses an untrusted ID.
Ledger bind is one-time even after all leases return. Native adapter must prove
that negative+zero output means no surviving registration/pins; this is currently
a trusted simulated-backend contract, not verified public syscall behavior.

registration-20261010T040736Z:13Rust cases opt0/opt2,4compile-fail+positivecontrol,
10runtime mutants. registration-c-20261010T040751Z:14C cases LLVM22ASan+UBSan and
optimized NDEBUG core,10runtime mutants. Five/seven source hashes and31/29raw
command records reverified; no late allocation on observed registration paths,
C allocation failure before native admission verified. REGISTRATION-REVIEW-R1
rejects guessed cleanup of malformed/colliding IDs; single-agent self-review in
owned-clients/registration-self-review.txt. Astra10hashes and clean61dde37 intact.
Native mapping/backing/syscall adapter, paired CQ/request correlation, fixed I/O,
futures and executor retirement remain; AS3/AS4/MM0-MM4 still pending. No Main,
private activation,128MiB change, independent audit or native-client pass claim.


### Astra request/CQ correlation, October 10

Uninstalled C/Rust request tables preallocate at existing CQ capacity (<=8192).
Library-wide monotone cookies reject cross-ring/stale completions; exact pool,
flags, receipt and length checks precede state changes. Prepared rollback ends at
SQ publication; ENTER errors never authorize storage reuse. MORE remains active,
terminal records stay charged until consumption, and payload receipts travel only
with delivery, independently of terminal metadata. Abandon requests scope-wide
abort once per request and retains ownership until terminal/retirement; actual
executor and storage owners remain to be integrated.

request-book-20261010T062909Z passed seven cases per language, Rustopt0/opt2,
Clone/Send compile failures with positive control, C LLVM22ASan+UBSan and optimized
NDEBUG core, and12runtime mutants. Ten source hashes/40raw commands reverified.
REQUEST-BUILD-R1 initial041455Z C build refused misleading indentation; preserved
failure, corrected loop formatting, no initial C runtime pass. Single-agent review
in owned-clients/request-self-review.txt records CQ-only terminal retention and
remaining native ownership boundary. Astra10hashes and cleanexternal61dde37 intact;
disk10041MiB before run,Mac released. No complete safe-client/AS3 claim, private
activation,Main,128MiB change or independent audit. AS4/MM0-MM4 still pending.


### Astra composed CQ/request/payload checks, October 10

Actual C/Rust paired reader, request book and shared pool lease cores now run
together in two host scenarios per language. CQ acknowledgment/reuse and final
consumption preserve held A while B returns; wrapper/CQ destruction leaves A's
owned backing intact. Abandoned MORE with failed return remains pending; malformed
pool identity is rejected after paired copy and explicit owner close retains A.
Producer, backing and retirement confirmation are simulated; no actual native
syscall, scope ABORT, fixed-I/O or future/executor qualification is implied.

composed-ring-20261010T063524Z passed Rustopt0/2, C LLVM22ASan+UBSan/NDEBUG and six
runtime mutants. Nineteen source hashes/30raw commands reverified. Initial063431Z
clean cases passed but mutant runner demanded FAILED summary when deliberately
missing live-count accounting caused an underflow/abort; COMPOSED-HARNESS-R1 raw
logs retained and exact expected diagnostic corrected. No full initial-suite pass.
Single-agent composed-self-review.txt records boundaries. Astra10hashes and clean
external61dde37 preserved; Mac released. Native owned mapping/backing/control and
composed controller/fixed-I/O/futures/executor remain, then AS3/AS4/MM0-MM4 gates.
No Main, private activation,128MiB protection change or independent audit claim.


### Astra owned mapping boundary, October 10

Uninstalled C/Rust owners now consume the mapping backend and validate the full
88-byte setup geometry before pointer access: header/SQ index/SQE/CQE/receipt
regions, exact counts/sizes, alignment, overlap, overflow and original mapping
identity. Fresh header checks precede exclusive paired-reader construction.
Failures close/destroy the original owner once; successful close is idempotent
and prevents further CQ access. No raw mapping/descriptor or clone API escapes.
Actual native setup/control/SQ publication and kernel pin retirement remain
trusted backend obligations, not a completed native adapter or AS3 qualification.

owned-mapping-20261010T064503Z passes Rust5 cases opt0/2,C6 scenarios LLVM22
ASan+UBSan/NDEBUG,3 ownership compile failures plus control,10 runtime mutants.
Eleven source hashes/snapshots and38raw command logs reverified before docs.
MAPPING-HARNESS-R1 initial064425Z clean profiles passed but custom case3 panic
missed runner's expected literal diagnostic; preserved evidence, fixed expected
Rust FAILED summary, full retry passed. Single-agent mapping-self-review.txt
records pointer/lifetime/cleanup review and remaining unsafe backend boundary.
Astra10hashes and cleanexternal61dde37 preserved; Mac released. Native adapter,
SQ publisher, composed controller/fixed-I/O/futures/executor remain before AS3,
AS4 and MM0-MM4. No Main, private activation or128MiB protection change.


### Operator-directed parking of SQ reuse correction, October 10

SQ-REUSE-R1 found shared sq_head publication before SQ-index/SQE copying; a
producer can reuse storage before the kernel snapshot. Proposed correction and
regression are preserved, unapplied, in astra-takeover/native-sq-reuse/sq-reuse.patch
with exact base, full patched source and PARKED.md. Operator reported a classifier
block and requested parking for Corona. No rejection reason was visible to this
turn; no bypass or assumption of future clearance is authorized. Only the four
agent-authored kernel changes were restored after complete patch preservation.

Already-launched native065006Z completed: five one-boot default1/4/8,UBSan4/8 rows,
1862tests each, D7overlap/20logins, min9944MiB. Raw source/diff/input/kernel/log
hashes reverified before restoring source. Host065053Z exact copy/release block
passed sanitizer/optimized profiles; old-order mutant failed cookie assertion.
Tests qualify only the parked patched source. Current build kernels include that
patch and MUST NOT be treated as matching the restored checkout. No full50boot,
complete AS3 or independent audit claim. Astra10hashes/cleanexternal61dde37 intact.

Continue independent fixed-I/O ownership, cancellation bookkeeping and bounded
executor retirement with simulated backends. Native SQ submission/integration
and complete async qualification depend on resolving the parked item. Async-first
then MM0-MM4 order and all activation/128MiB/Main gates remain. Heartbeat prompt
updated with exception; its existing PAUSED status preserved.


### Astra independent fixed-I/O ownership, October 10

Uninstalled Rust/C fixed operations now share the existing ring backing owner.
Checked reservations exclude provided pools in both directions, permit WRITE
copy-in only before publication, and expose READ bytes only after a validated
terminal result. Active drop requests scope cancellation while retaining storage;
failed ENTER has no rollback path. Confirmed terminal or whole-owner retirement
reclaims abandoned reservations. Live terminal views retain backing past wrapper
close. One preallocated reservation per registered buffer (<=64) is a conservative
library limit; actual request accounting still requires RequestBook integration.

Rust fixed-io074052Z:20cases opt0/2,6compile-fail+control,8mutants. C fixed-io-c074317Z:
21scenarios LLVM22ASan+UBSan/NDEBUG,8mutants. Composition074349Z:two scenarios per
language, bothprofiles,6mutants. Five/seven/nineteen source hashes and29/25/30raw
commands/snapshots reverified; single-agent fixed-self-review.txt records lifetime,
callback, aliasing, cancellation and release checks. All backends, terminal and
retirement proofs are simulated; typed futures/native request integration and
bounded executor retirement remain. WRITE cancellation never implies remote rollback.
Astra10hashes/cleanexternal61dde37 preserved. SQ reuse remains parked and unapplied;
no native gates rerun, Main landing, private activation or128MiB change. Heartbeat
remains paused as found. AS3/AS4/MM0-MM4 remain incomplete.


### Astra typed fixed request controllers, October 10

Uninstalled C/Rust controllers couple preallocated request metadata and fixed
storage through exact library cookies. Preparation failures release both
reservations; active abandonment requests scope abort once and retains ownership
until authenticated terminal or whole-owner retirement. READ results own backing
past driver close; WRITE results preserve actual errors. Malformed/foreign records
close only the affected driver. No allocation in receive/completion/drop/progress.
TYPED-REVIEW-R1 fixes prepared/active polling after driver drop to report Closing
while retaining storage, rather than waiting indefinitely without a driver.

Rust083816Z:10cases opt0/2,4compile-fail+control,6mutants,8hashes/23commands.
C083803Z:11scenarios LLVM22ASan+UBSan/NDEBUG,6mutants,13hashes/25commands.
Raw logs/source snapshots and current diff reverified before documentation;
initial nine-case Rust083141Z retained. Single-agent typed-self-review.txt records
ownership/error/retirement boundaries. Protected10/cleanexternal61dde37 and parked
kernel base preserved. These are typed polling handles, not Future/waker/executor
or native SQ integration. Continue bounded executor retirement/futures independently;
SQ correction remains parked/unapplied. Heartbeat resumed ACTIVE under operator's
latest autonomous-continuation instruction. AS3/AS4/MM0-MM4 remain incomplete;
no Main, private activation or128MiB protection change.


### Astra bounded library retirement owners, October 10

Uninstalled C/Rust executors reserve a bounded retirement cell before the driver
factory runs. Dropping a managed client transfers it to its existing cell without
allocation; confirmed local retirement and ended storage users are required before
reuse. A rotating progress round visits at most min(budget,capacity) cells, keeping
busy entries charged while allowing later ones to finish. Root shutdown closes
descriptors once, rejects new operations and preserves existing payload backing.
The1..64cell bound is conservative library configuration, not a new kernel quota.

Eight scenarios/language pass: Rust084435Z opt0/2,2compile-fail+control,6mutants;
C084418Z LLVM22ASan+UBSan/NDEBUG,6mutants. Nine/fifteen source hashes and21/27raw
command records reverified. Initial084249Z Rust clean profiles passed; final
mutant failed correctly with unreachable-factory panic, but diagnostic expected
assertion. RETIREMENT-HARNESS-R1 evidence retained, expected diagnostic corrected,
complete retry passes. Single-agent retirement-self-review.txt records ownership,
allocation, shutdown and fair bounded progression. Protected10/cleanexternal61dde37
and parked kernel base unchanged. Event-loop wakeups/typed Future integration and
native backend remain pending; this is not a full async executor/AS3 pass. SQ stays
parked/unapplied. Heartbeat ACTIVE; AS4/MM0-MM4 and all activation guards remain.


### Astra Rust fixed Futures and bounded wake registrations, October 10

Uninstalled Read/Write Futures own typed requests and reserve one of64wake cells.
Preparation errors roll back registration; Future drop unregisters without
releasing active storage. Terminal delivery wakes a bounded snapshot; polling
rechecks readiness and READ results retain backing. Waker clone precedes readiness
check, replacement precedes old-Waker drop, and clone/drop/wake callbacks execute
outside internal borrows. Root close wakes unfinished tasks while preserving
owned storage. No internal allocation on observed Arc-waker poll/receive paths;
arbitrary caller Waker implementations can allocate and are outside that claim.

fixed-futures085203Z:12cases opt0/2,2compile-fail+control,6mutants;9hashes/21raw
commands verified. Eleven-case085056Z pass retained before full64cell regression.
FUTURE-REVIEW-R1 records reentrant clone/drop/wake checks and lost-wakeup mutants.
Single-agent futures-self-review.txt; protected10/cleanexternal61dde37 and parked
kernel base unchanged. Integration with the bounded retirement owner is next:
notifications must escape every enclosing manager borrow before invocation.
Native event/sleep/deadline integration remains pending; no complete async executor,
AS3 or C callback qualification is claimed. SQ stays parked; heartbeat ACTIVE,
AS4/MM0-MM4 and all Main/private-activation/128MiB guards remain.


### Astra Future/retirement ownership composition, October 10

One generic Rust retirement queue now supports polling and Future drivers with
preallocated cells. Completion, client drop and executor shutdown return deferred
wake batches, dispatched after all enclosing borrows end. Progress detaches ready
entries before driver destruction. Callback tests poll tasks and drop/access peer
clients safely. Normal client retirement preserves an already-terminal WRITE
success; held READ results still block cell reuse until their users finish.

Composition090037Z:6cases opt0/2,2compile-fail+control,6mutants,10hashes/21commands.
Polling retirement regression090048Z:8cases,2compile-fail+control,6mutants,
10hashes/21commands. Future regression090111Z:12cases,2compile-fail+control,
6mutants,9hashes/21commands. Raw source/diff/logs reverified. Initial090001Z clean
profiles passed; borrow mutant aborted during destructor cleanup after exact
RefCell panic, so expected FAILED footer was absent. FUTURE-RETIRE-HARNESS-R1
preserves failure and corrects expected diagnostic; complete retry passes.

Single-agent future-retirement-self-review.txt records composition and remaining
local-progress duty. Local event/sleep/deadline integration is next, including
notification when the last held result drops; callers currently drive progress
explicitly. C unchanged. Protected10/cleanexternal61dde37 and parked kernel base
preserved. No native SQ or full AS3 qualification; heartbeat ACTIVE, AS4/MM0-MM4
and Main/private-activation/128MiB guards remain.


### Astra creator-local retirement progress, October 10

The uninstalled Rust owner now signals a coalesced creator-thread pulse when a
request/result or payload releases ownership, a pool/client retires, or a fixed
terminal completion arrives. The bounded retirement queue consumes each pulse
as one rotating sweep; zero and partial budgets preserve pending work. Busy
entries become quiet until another event, avoiding an idle retry loop. A held
READ result can release the last local owner and enable retirement without a
new CQ. Existing retirement proof and storage-lifetime requirements still apply.

Local-progress091022Z:7cases opt0/2,2compile-fail+control,6mutants. Composition
regression091254Z:6cases; polling retirement091311Z:8cases, each opt0/2 with
2compile-fail+control and6mutants. Each run has10source hashes/21commands;
source/snapshot/HEAD/diff and raw diagnostics verified before documentation.
Single-agent local-progress-self-review.txt records event coalescing, bounded
sweeps, lifetime and callback checks. No new test/harness failure. C unchanged.

Next is local event-loop/sleep/deadline integration with simulated events.
This pulse performs no OS wake and is creator-thread only. Callers still invoke
progress_notified and check needs_progress before sleep; backend readiness and
retirement changes need explicit notification. No native or full AS3 gate is
claimed. Protected10/cleanexternal61dde37 and parked kernel base preserved.
SQ stays parked/unapplied; AS4/MM0-MM4, Main/private activation and128MiB guards
remain. Heartbeat ACTIVE under continuing operator authorization.


### Astra simulated local wait/deadline coordination, October 10

The uninstalled Rust coordinator performs one bounded retirement turn and at
most one wait. It registers interest before rechecking external readiness/time,
then checks local ownership pulses last. An armed guard cleans up every return
path; pending partial work prevents sleep. Spurious wakes retain the original
absolute deadline and schedule progress. Timeout reports expiry without treating
it as terminal completion or allowing storage reuse.

local-wait091938Z:9cases opt0/2,2compile-fail+control,6mutants;11source hashes and
21rawcommands/snapshots/HEAD/diff reverified before documentation. Owner drops
during arm/readiness, event between recheck/wait, absolute deadlines, bounded
budgets, error cleanup and retained active ownership are covered. No new failure.
Single-agent local-wait-self-review.txt explains the limits: simulated event
source only, no native atomic wait or actual sleeping-thread qualification.
The future adapter must retain events until serviced and include runnable/CQ
work. Existing ownership cores and C code are unchanged.

Next independent bounded per-request deadline/cancellation bookkeeping and
runnable-task composition. Protected10/cleanexternal61dde37 and parked kernel
base preserved. SQ remains parked/unapplied, native/fullAS3 qualification gated;
AS4/MM0-MM4, Main/private activation and128MiB guards remain. Heartbeat ACTIVE.
