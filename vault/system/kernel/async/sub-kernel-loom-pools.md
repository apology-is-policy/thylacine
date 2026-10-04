---
id: sub-kernel-loom-pools
type: sub
parent: moc-kernel-async
title: "Private Loom pool storage and exact payload leases"
code:
  - kernel/loom_service_pool.c
  - kernel/include/thylacine/loom_service_pool.h
  - kernel/test/private_pool_fixture.h
  - tools/host-tests/loom-service-pool.c
  - tools/test-loom-service-pool.py
audit: hard
guarded-by: [inv-i29, inv-i30, inv-i32]
validated-by: [spec-loom-service-buffers]
locks: []
abis: [abi-loom-service]
design:
  - "docs/ASYNC-SERVICE-BUFFERS.md"
created: 2026-10-04
updated: 2026-10-04
---

## Purpose

The internal C pool module implements bounded metadata transitions without
allocations, callbacks, usercopy or internal locks. It is compiled into the
kernel and exercised by the existing Loom buffer-registration test. Private
syscall setup is still disabled; there is no live ring consumer of this module.
The owner must hold its ring lock and retain canonical Burrow pins throughout.
It must resolve authority, mapping ranges and exclusion against accepted fixed
I/O before prepare. Those are caller obligations, not proven by this helper.

## Data structures

One zero-initialized loom_pool_bank per ring has64 cells,5128bytes including
its monotonic last-issued64-bit nonce. Each cell is80bytes: full pool reference,
canonical backing pointer/offset/length, lease nonce, user_data, member ordinal,
phase, requested/result lengths and MORE. Each32-byte loom_pool descriptor
belongs in the shared64-slot service table; this module adds no separate slot
registry. A48-byte transient result carries a32-byte receipt and completion
metadata. Static assertions pin all four sizes. These sizes are NOT a charge
or allocator footprint measurement; owner allocation/accounting remains owed.

Members have FREE, AVAILABLE, BUSY, PENDING, LEASED phases. Pool descriptors
are EMPTY, RESERVED or LIVE. The bank's nonce is never reset by pool removal.
UINT64_MAX can be issued once; a further claim returns ENOSPC without wrapping,
even if all members are held. Failed/aborted shots burn their nonce.

## Contract

All APIs below require caller serialization. No helper wakes a peer or progress
thread under that lock. Returning payload merely changes metadata; its owner
must issue any necessary wake after unlocking.

- prepare(bank,pool,ref,extents,count): validates the whole set before mutation,
  then reserves quota and extents while the slot remains unpublished. Extents
  compare canonical backing identity and checked exclusive byte ranges, not VAs.
  Requires0<count<=64 and sufficient combined bank space. Caller has already
  pinned backing and excluded earlier accepted fixed operations. Invalid input
  is EINVAL, occupied slot/overlap with existing pool EBUSY, quota ENOSPC.
- conflicts(bank,extent): includes RESERVED cells. Invalid extents conflict,
  preventing overflow or zero-length input from bypassing exclusion.
- publish(pool): RESERVED->LIVE after successful output copy; else EINVAL.
- rollback(bank,pool): RESERVED only; clears its cells and descriptor, preserving
  siblings and global nonce. Caller burns slot incarnation and drops pins after
  exclusion removal; no hidden registration survives a copyout failure.
- stream_get/put(pool): retain pool from accepted request through its terminal
  bookkeeping. Get requires LIVE and prevents32-bit wrap; put rejects underflow.
- claim(bank,pool,maximum,receipt): requires a live referenced pool and a positive
  signed-CQE-sized maximum fitting EVERY member. Scans from a member cursor,
  claims only AVAILABLE and mints a nonce before I/O. EAGAIN means wait locally,
  never emit a Tread. This is member rotation, not inter-stream scheduling.
- release_busy(bank,pool,receipt): caller has ended all local writers before
  returning a BUSY member. Exact nonce and full pool reference required.
- commit(bank,pool,receipt,length,user_data,more): positive result no larger than
  claimed maximum; BUSY->PENDING preserving full correlation. Caller serializes
  payload copying and cancellation before this commit. EOF/error uses release_busy
  and the request's separate terminal record, never a zero-byte payload lease.
- peek(bank,pool,member,result): copies the stored PENDING result without consuming
  it. Full CQ therefore needs no allocation or overwrite of committed payload.
- deliver(bank,pool,receipt): PENDING->LEASED only after copying CQE and receipt,
  before release-publishing CQ tail under the same owner lock. This function
  does not itself publish shared memory or prove weak-memory ordering.
- return(bank,pool,receipt): LEASED->AVAILABLE only for exact pool incarnation,
  member and nonce. Pending/BUSY/stale/double returns fail; never trust the shared
  receipt mirror. Bad shape/index is EINVAL, stale identity/state is ENOENT.
- snapshot(bank,pool,out): one consistent live snapshot; clears unused fields,
  reports member phases and stream count. It neither publishes nor recycles.
- reap(bank,pool): EBUSY until streams==0 and all members AVAILABLE. It clears
  only that pool. PENDING/LEASED cannot be reclaimed by source retirement.

No CQ-head API exists. Reaping a completion does not call into this module and
cannot return a payload. Whole-ring destruction must first stop actual kernel
writers; it is not implemented by calling reap on a still-active pool.

## Prosecution

Claim/Reply/Deliver/Return in [[spec-loom-service-buffers]] map to claim/commit/
deliver/return here. Stop maps to release_busy after writer quiescence. Source
Finalize/Retire and paired CQ stores remain caller integration obligations.
Physical mapping pins, fixed-I/O admission, usercopy and source locks remain
outside this helper's verified boundary.

The actual module links directly into tools/test-loom-service-pool.py's host
fixture, with ASan/UBSan and eleven source mutations. Assertions cover canonical
aliasing, provisional exclusion/quota, rollback, retained byte patterns A/B/C,
out-of-order/stale/double returns, pending/source-retirement retention, result
correlation, nonce exhaustion and reap refusal. The shared native fixture returns
errors to the outer TEST_ASSERT, avoiding nested void assertions that hide a
subtest failure. Mutants must fail the exact named assertion, not compilation.

Evidence and native build outcome live in docs/ASYNC-SERVICE-STATUS.md and
work/oct4-async-service/buffer-pools. No throughput, multi-stream fairness,
graphical result, syscall activation or complete private-I/O claim follows.

## Mechanism

Registration has a validate pass followed by an infallible reservation pass.
Cells store their pool reference and local ordinal. Lookups scan at most64
cells, keeping quota and physical overlap global to this ring without a second
pool-slot registry. A result remains in its cell until publication/return.

## Concurrency

No independent lock domain: every operation belongs under the caller's ring
lock. Pool publication is separate from prepare so usercopy may run unlocked
while provisional exclusion remains installed. Writers must end before BUSY
release; callback serialization and wake-after-unlock belong to the owner.

## Invariants enforced

Combined member capacity, nonoverlapping canonical extents, exact phase/receipt
validation, no nonce wrap, no result larger than its request, no reap of retained
work, and rollback without touching siblings. CQ consumption has no mutation
entry point. The model and exact named counterexamples prosecute these rules.

## Error paths

Invalid shapes/ranges/phase controls use EINVAL; stale receipts or unpublished
pools use ENOENT; reserved extents, active refs or payloads use EBUSY; quota and
nonce exhaustion use ENOSPC. Empty pool is EAGAIN. No failure allocates or calls
out. prepare errors leave bank/pool untouched; copyout rollback is explicit.

## Performance

All scans are bounded by64 cells. prepare checks at most64x64 member overlaps;
claim's cursor search is at most64x64 comparisons. No measured latency claim.
Member rotation does not establish fairness between streams. Exact structure
sizes above are compiled assertions, not actual allocator/payer charges.

## Seams

Private ring owner, shared slot table, Burrow pin/charge ledger, accepted fixed
I/O exclusion, ordered completion publication, close/exec reaper and safe userspace
lease wrappers must be connected before activation. No seam is declared closed
by a successful pure metadata test.

## Caveats

Extent backing is a trusted pinned canonical object, never a userspace pointer.
The module neither pins nor checks mapping ownership. Raw alias self-corruption
is a caller obligation under the approved design. Whole-ring destruction needs
its own writer-drain path; scope retirement alone cannot release payloads.

## Provenance

Operator-selected option C, scripture30695b43e, ABI3eb14ae73 and modele6a50ae1e.
AS-2e implementation and native/host evidence are recorded in
ASYNC-SERVICE-STATUS.md. Review staffing is explicitly single-agent.
