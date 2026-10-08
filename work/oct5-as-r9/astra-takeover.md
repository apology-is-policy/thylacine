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
   Compile/run remains pending the resource window; no guest result exists.
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

At takeover Main's groupstatus gate had live registered processes and its own
Mac lease; it remains untouched despite the agents' credit exhaustion. Aux was
queued. Astra has no lease or running compiler/VM. Disk was 7.9 GiB and the
inherited runner requires 8 GiB. Recheck before heavy work, never lower the floor
or acquire a lease just to wait for disk. Acquire/release only Astra's lease;
never modify another agent's lease or stop their jobs. No new cleanup is approved.

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
Corona/Astra review. Current coordination cursor: turn 83, note 55.
