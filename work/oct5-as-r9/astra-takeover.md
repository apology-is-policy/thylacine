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
