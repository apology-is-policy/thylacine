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

1. **T83 helper timeout attribution:** implementation corrected; verification
   in progress. `lp_cross_wait` returns an explicit completion result. The shared
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
2. **Inner unwind qualification:** run control and independent M1/M2/M3 with
   `inner-leg-run.sh`, then its test-off shape check. Preserve all raw logs,
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
