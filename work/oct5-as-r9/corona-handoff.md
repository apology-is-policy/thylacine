# Corona implementation handoff — October 5

The operator explicitly authorized Corona to implement the resumed async
service lifecycle and production memory-accounting work, with Astra giving
the implementation handoff and coordinating through Yip. This permits our
collaboration; do not spawn additional agents from the older general audit
instructions. Work as corona in your own checkout. Astra will review your
changes and integration evidence and will not edit the same implementation
surfaces concurrently. Please acknowledge the base and scope, then proceed.

## Base, authority and coordination

Your intended starting commit is Astra
`5ff62b78809846af4780ec41f82d1676e7584e80`, on your branch
`corona/async-memory` in `/Users/northkillpd/projects/thylacine-corona`.
Verify it and your initial status before editing. Do not blindly merge current
Main: Main has newer reader-role/lifecycle work that has not been reconciled
with Astra's private-service foundations. Coordinate with Main/Aux before
overlapping shared surfaces or importing a new base. Astra already notified
them on calls 0116 and 0108; those notes do not constitute their clearance.

Read AGENTS.md, CLAUDE.md, the relevant architecture invariants/dossiers and:

- docs/ASYNC-MEMORY-DESIGN-REVIEW.md
- docs/ASYNC-SERVICE-LIFECYCLE.md
- docs/ASYNC-SERVICE-STATUS.md
- docs/ASYNC-SERVICE-BUFFERS.md and its concrete ABI declarations
- docs/SHARED-MEMORY-ACCOUNTING.md
- docs/HALCYON-INTERACTION-STATUS.md
- docs/DEBUGGING-PLAYBOOK.md, docs/agent/SPEC-POLICY.md and relevant audit rows.

The October 4 async/memory designs, async-first order and provided-buffer
option C are approved. The operator's October 5 request to retry explicitly
supersedes the earlier park of this work. Retain the 128 MiB protection until
the replacement passes its activation gates. Keep private async and clipboard
nondefault until their full qualification; no new policy or ABI beyond the
approved contracts without identifying the design fork. Do not weaken checks.

Use `yip beat` during active work, `yip busy` for useful presence, and your own
Mac/Pi lease before substantial host work. Queue normally, release promptly in
finally, and never touch another agent's lease, jobs, checkout or artifacts.
Astra currently holds no resource and will stop refreshing its old AS-R9 queue
request. Request your own lease; queue ownership is not transferable.

## 1. Reproduce and repair AS-R9 first

This is a source-identified race, not a measured guest failure yet. No
production lifecycle code was changed by Astra during this retry. No AS-R9
host test has run. The problem is the three-operation sequence:

1. claim/clear the Burrow's charge record;
2. drop a handle or mapping reference; the drop reports nonfinal;
3. restore the charge through the Burrow pointer.

Another holder can perform the final release between steps 2 and 3. Separate
Burrow-lock acquisitions protect each operation, but not the interval. The
first holder no longer owns a reference when it restores. The second holder
can also observe the temporarily empty charge record and miss settlement.

Inspect all callers, not just the dormant private owner:

- kernel/loom.c: loom_drop_pin_settling and displaced registered-buffer pins.
- kernel/weft.c: explicit share unregister and weft_share_release_owner.
- kernel/vma.c: whole eager-ANON detach in vma_detach_range_in, with
  vma_free_deferred / burrow_release_mapping_deferred.
- kernel/syscall.c: JIT destroy also uses claim/restore, but holds the address
  space lock and a surviving alias on failure. Prove its remaining-reference
  contract separately; do not assume every claim/restore caller is unsafe.
- kernel/test/test_addrspace.c has existing exact-payer claim tests.

Read kernel/burrow.c and kernel/include/thylacine/burrow.h. Preserve the
dual-reference zero/zero destruction rule, exact AddrSpace incarnation payer,
deferred FILE teardown outside address-space locks, and current shared-out
refund behavior. Do not silently introduce the replacement accounting policy
inside this prerequisite fix.

Recommended implementation shape, subject to your source review:

- Add an internal exact-payer settled handle-drop operation: under the same
  Burrow lock, decrement the reference, decide finality, and claim the matching
  charge only if this release qualifies. On a nonqualifying release leave the
  charge record intact. Return the scalar refund to the caller; no restoration
  or other Burrow access after losing its reference. Free outside the lock.
- Add the corresponding deferred mapping-drop operation. Preserve the current
  eligible eager-ANON/shared-out policy with the shared_out observation and
  charge claim inside that same lock. Return dead storage for deferred freeing
  and scalar pages separately. Refund outside the Burrow leaf lock.
- Migrate the affected Loom/Weft/VMA callers. Specify caller ownership of the
  exact payer descriptor; a private asynchronous owner must retain its own
  AddrSpace pin rather than discover a dead Proc or recycled PID.
- Narrow/document any retained claim/restore API to callers that provably
  retain an independent reference for the whole interval. Update obsolete
  comments describing the window as merely benign overcharging.

Astra prepared an UNRUN controlled reproduction scaffold in:
`/Users/northkillpd/projects/thylacine-astra/work/oct5-as-r9/`
Files: base.json, old-source.c, reproduce.py. You may read/copy these into your
own evidence directory; do not run them in Astra's checkout. The fixture
extracts actual pre-fix source and schedules the final holder at unlock. Its
allocator double poisons the descriptor to identify a stale restore without
dereferencing freed host memory. Review the fixture before relying on it.

Required focused evidence: reproduce the old handle and mapping schedules;
verify the repaired schedules, both drop orders, mixed handle/mapping holders,
wrong payer/old incarnation, already-settled records, retained nonfinal charge,
exactly-once final refund, shared-out handling and deferred frees outside the
leaf lock. Use intended source mutants with named failing assertions, native
tests and relevant existing model counterexamples. State the boundary between
host doubles and native behavior. Do not call an unrun fixture a reproduction.

Host sanitizer note: prior Apple Clang 17 ASan startup hung before main;
Homebrew Clang 22.1.4 with the current SDK via scoped CC/CFLAGS worked. Inspect
existing successful runners instead of globally changing compiler settings.
The October 1–2 gate waiver expired; normal applicable gates are required.

## 2. Resume private service ownership and retirement

After AS-R9 is qualified, review the preserved UNBUILT owner draft:
`/Users/northkillpd/projects/thylacine-astra/work/oct4-async-service/owner-integration/paused-owner/`
It includes pin.json, base files, modified files and patch.diff. Its original
base is c822021a2ea56a452b4cdbe7709e6fa117a7678b. It is a draft, not a patch
to apply blindly. Review it against your repaired base and the approved design.
Read neighboring plan.md, pickup.md and owner-diagnosis.md for context.

Already committed foundations include resumable private 9P transport,
AddrSpace descriptor pins and private-ring sharing guards, transactional
provided-buffer pool core, worker tickets, preallocated protocol storage and
paired CQE/32-byte receipts. Public private-feature masks remain disabled.

Finish in coherent reviewed checkpoints:

1. Private owner construction, admission, bounded tables and metadata charges;
   cover all refusal/unwind edges before publication.
2. Close/exec/reaper and worker retirement: exact retained identities, no new
   admissions after close, terminal publication after local resource retirement,
   no premature owner/pool/payload reuse. Review handle duplication/transfer,
   rfork/RFMEM exclusions and all creation/exit entry points.
3. Scope protocol, cancellation/deadline ordering, provided-buffer receipts and
   return semantics. Preserve bounded progress and independently held payload
   leases; consuming a completion is not returning its payload buffer.
4. Safe owned client interfaces and real isolated runtime lifecycle tests,
   then qualified service adoption. Report activation separately from a
   dormant implementation checkpoint.

Do not redo old broad gates merely to restate old evidence; do run the gates
appropriate to new lifecycle source. Preserve paired kernel/ramfs/pool inputs,
logs, actual individual matrix classifications and cleanup on failures.

## 3. Production memory accounting, in the approved order

Follow the MM0–MM4 phases in SHARED-MEMORY-ACCOUNTING.md:

- MM0: exhaustive allocation/retention/alias/driver inventory, concrete ABI
  mirrors before consumers, and explicit charge/rollback ownership contracts.
- MM1: durable hierarchical accounts in shadow mode; immutable parents and
  nonrepeating incarnations, reserve/commit/cancel transitions, depth/resource
  bounds, orphan accounting and lifetime independent of Proc storage.
- MM2: cover all retention paths and restricted session-funded server vouchers.
  Keep physical backing counted once, durable sponsorship separate from each
  account's independently retained backing, and mapping/metadata bounds separate.
  Subranges retaining whole backing must retain its whole charge. Distinguish
  HOSTMEM aperture from actual RAM; device fences precede storage refunds.
- MM3: cooperative pressure notifications and recoverable graphics admission;
  preserve the previous working graphics generation when replacement fails.
- MM4: remove the legacy 128 MiB stopgap only after the new enforcement and
  lifecycle/migration gates are qualified. No speculative early activation.

Use the approved limits, authority split and lock ordering. Do not allocate,
sleep or perform page work under the account transaction lock; do not take it
under Burrow/VMA/driver locks. Escalate genuine design forks with a concrete
reviewable proposal instead of silently changing the approved contract.

## Delivery and review

Update owning Vault dossiers, status, journal and evidence with actual results;
render/lint and commit through normal hooks. Do not disable verification to get
a commit. Send Astra each coherent commit SHA, changed surfaces, test logs,
model/mutant verdicts, remaining limits and self-review findings through this
call. Keep a durable pickup in your work directory. Astra will review before
integration; do not land Main or push an integration into another checkout.

The four separate authority/settings drafts live only in Astra's working tree
and must stay there unchanged. You do not need to import or stage them. Your
new local Yip/Claude configuration also does not belong in implementation
commits. Do not copy Astra's generated/build artifacts as if they qualify your
new source. Report real runtime outcomes and screenshots only when exercised.
