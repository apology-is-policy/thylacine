# Astra takeover, October 8

The operator transferred Corona's entire remaining implementation and runtime
qualification to Astra: "All Claude agents now out of credits. You can take over
Corona's work entirely now." This supersedes the prior split of implementation
and review ownership. Continue in this checkout on `corona/async-memory`; the
takeover base is `eb03e4f65fa230d8ab66ed792b1626639fadabf9`. Its tracked files were
clean. Preserve its untracked evidence and pinned input generations. Astra's
permanent checkout and ten pending/protected files remain separate and untouched.

Operate Yip only from `/Users/northkillpd/projects/thylacine-astra` as Astra.
`astra-yip.sh` does this for the runner's resource checks; it does not override an
identity. Set `THYLA_WAKE_YIP` to its absolute path when running the inherited
runner. Do not operate Yip as Corona. The handoff's approved async-first then
MM0-MM4 work order remains binding. No Main integration, unfinished activation,
or early removal of the 128 MiB protection follows from this transfer.

## Work queue

1. **T83 helper timeout attribution:** correction verified by seven host cases
   and the normal-completion guest control. `lp_cross_wait` returns an explicit completion result. The shared
   void join is called only after the helper's terminal publication. Timeout
   disarms the watch and fails before the inner leg, without reclaiming a
   transferred object or potentially running helper. A late terminal publication
   cannot change the captured timeout result. The static handoff remains alive;
   no second fixture invocation is supported after this failed boot.
   `cross-timeout-control.py` extracts the actual wait/helper/control block and
   compares the old and new block with host scheduler/allocation doubles.
   Host control executed on October 8: the old block loses the timeout (expected
   rc 11); all seven repaired cases pass. Kernel syntax has zero errors, two
   pre-existing prototype warnings. No new guest result exists.
2. **Inner unwind qualification:** COMPLETE at bounded Mac single-boot scope
   in inner-leg-20261008T195301Z (see final entry below). The control and
   independent M1/M2/M3 were run with
   `inner-leg-run.sh`, including its test-off shape check. Preserve all raw logs,
   exact source, mutations, nine matching boot inputs and restored control
   artifacts. The new log check requires exactly the expected failure diagnostic
   as well as the final verdict, so a helper timeout cannot be overwritten and
   mistaken for discrimination. Constructed-log arms: 26/26 passed, retained at
   `astra-takeover/inner-arms.txt`; this is checker evidence only.
3. **Remaining private-owner coverage:** release sensitivity remains open;
   `checkpoint1-coverage.md` distinguishes runtime, structural and engine-owned
   obligations. Do not redo qualified old legs without a relevant change.
4. **New-source lifecycle qualification and engine:** applicable native/SMP
   and sanitizer gates, then bounded engine tables, close/exec/reaper and worker
   retirement, scopes/cancellation/deadlines, independent payload leases and
   owned clients, real service adoption. Follow `corona-handoff.md` and existing
   designs. Reconcile recorded merge obligations only when integration is in
   scope; no blind Main import.
5. **Production accounting:** MM0 inventory/ABI contracts, MM1 shadow accounts,
   MM2 retention and vouchers, MM3 pressure/recoverable graphics, MM4 activation
   only after its gates. Existing protection remains until then.

## Resources and verification

The operator subsequently said: "Main, Aux, Corona are all out of credits until
Monday -- all resources are free, as they cannot use them. Feel free to take any
at will." This explicitly supersedes prior peer-lease waiting restrictions.
Astra may reclaim resources without waiting for those agents. Record resource
use as Astra; do not impersonate a peer or silently falsify lease checks.
Already-launched background jobs can still run despite exhausted credits:
Main's groupstatus/SMP gate remained live at this check and was preserved.
The tiny host regression and syntax check ran under this operator grant.
The 8 GiB runner disk floor remains binding (7.2 GiB observed). No files were
deleted or peer leases changed in this checkpoint. Recheck before guest work;
resolve actual disk capacity and resource bookkeeping rather than bypassing
runner checks. Keep all source and pinned evidence intact.

Once resources allow, from this checkout:

```sh
export THYLA_WAKE_YIP="$PWD/work/oct5-as-r9/astra-yip.sh"
python3 work/oct5-as-r9/cross-timeout-control.py
sh work/oct5-as-r9/syntax-check.sh kernel/test/test_loom.c
sh work/oct5-as-r9/inner-leg-run.sh
```

The first two must pass before the guest runner. The runner requires a clean
tracked tree; commit the source and current documentation through normal hooks
first. Keep the exact clean Stratum source `61dde3727921e70e2c72fbd3c9e2044a192f4a54`
at `/Users/northkillpd/projects/stratum-astra/v2`. Prior full Mac qualification
and later single-boot checkpoints retain their original scopes and source pins.
This takeover has not repeated them. Self-review is single-agent, not independent
Corona/Astra review. Coordination through turn 83; operator resource override and host results recorded separately.


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
