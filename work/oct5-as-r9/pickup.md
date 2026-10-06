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
