# Private Loom provided-buffer pools

October 4, 2026. Operator selected option C: implement explicit buffer pools now.
This concretizes that approved direction before ABI mirrors and consumers.
Canonical contract for the approved pool extension. Runtime remains disabled.

## Ownership, not completion-queue retention

A pool is a private-ring object containing bounded slices of already registered
anonymous buffers. Before sending a pooled Tread, the kernel reserves one free
slice and its completion storage. A successful response copies once into that
slice and hands a new payload lease to the application. The kernel cannot write
that slice again until an explicit return validates the pool incarnation, member
index and exact non-repeating lease number. Reaping a CQE never returns payload.

The payload and its completion have different lifetimes. Keep the existing
64-byte SQE, 16-byte CQE and user_data correlation. Add a 32-byte receipt beside
each CQ slot, in the pool-enabled ring mapping. The consumer copies the CQE and
receipt together before advancing cq_head. It can then release CQ capacity while
retaining the payload arbitrarily long, within already admitted memory limits.
No borrowed payload forces unrelated completions to remain in the CQ.

The receipt carries full 64-bit identities. Do not truncate a generation into
CQ flags, query a mutable current buffer record after releasing its CQ slot, or
replace user_data with an unrelated kernel token. Those alternatives either
permit stale association or silently change completion correlation.

AS-R7 is a design gap discovered before private mode activation. Legacy READ
MULTISHOT already fails; working legacy scalar multishot remains unchanged.
The AS-2 transport/owner prerequisites are independent of this new contract.

## Precedents and fit

Plan9 pairs Tread with one Rread carrying bytes. Multishot remains a local Loom
resubmission mechanism over those ordinary pairs, not a new server protocol.
https://9p.io/magic/man2html/5/read

Genode packet streams hand a shared-buffer descriptor to a consumer and return
ownership through acknowledgement after processing. This is the closest
capability-system ownership precedent. We retain Thylacine's native registry,
scope and Burrow rather than introducing Genode's transport.
https://genode.org/documentation/genode-foundations/25.05/architecture/Inter-component_communication.html

Fuchsia registers reusable VMOs and separates small control messages from bulk
storage; registration itself does not establish per-payload reuse permission.
https://fuchsia.dev/fuchsia-src/development/drivers/best_practices/vmo-registration-pattern

liburing multishot reads select provided buffers. The completion identifies the
selected buffer and the application explicitly returns it after processing.
We adopt that separation with full incarnation/lease identities and use existing
REGISTER control calls rather than adding a producer ring in this first version.
https://www.man7.org/linux/man-pages/man3/io_uring_prep_read_multishot.3.html
https://kernel.googlesource.com/pub/scm/linux/kernel/git/axboe/liburing/+/refs/heads/master/man/io_uring_provided_buffers.7

## Bounds and authority

Pools consume slots from the existing shared 64-slot target/scope/fid envelope;
POOL is kind4. At most64 member slices exist across all pools in a ring, within
the existing64 registered-buffer limit. One buffer may be partitioned into
several disjoint members, but partitions consume the same64-member envelope.
No new capability, registry route, process identity or transferable handle.

Registration takes an immutable copy of a fixed-size member array, resolves
registered buffers under ring ownership, range-checks with checked arithmetic,
and pins canonical Burrow objects/byte extents. Reject zero lengths, out-of-range
indices, nonzero unused fields, and overlapping extents within/across pools in
this ring, including aliases registered at different virtual addresses. Compare
backing identity and physical byte interval, never just buffer indices or VAs.
Reject the ring's own Burrow as pool backing. Existing ANON/RW/contiguity gates
remain; no lazy/FILE/device backing expansion is implied.

Installing a pool also refuses overlap with any already accepted fixed-buffer
operation. Provisional registrations reserve both member capacity and extents
before releasing the ring lock for output copy: a competing registration or
fixed-buffer submission must already see that exclusion, even though the new
pool reference is not usable. Rollback removes those exclusions and pins.
While pooled, those slices cannot be named by ordinary fixed-buffer
READ/WRITE/WALK or other payload operations on this ring. Buffer-table replacement
returns EBUSY while any pool exists. This deliberately makes registrations stable
for the lifetime of a pool and avoids a hidden rebinding identity.

The application must not concurrently mutate/read a kernel-owned slice or lend
it to another I/O engine. Raw userspace can already write its own mappings; pool
registration is not protection against self-corruption through CPU/cross-ring
aliases. Safe Rust/C wrapper ownership must reflect this requirement. Existing
COW rules remain: writable eager-ANON backing can cause fork refusal; only ring
VMAs receive PRIVATE_RING omission. Do not hide user buffers from a child or
claim that every process with registered buffers can fork successfully.

## Reserved encoding

These numbers require the normal ABI-registry/mirror commit before consumers;
they are not enabled by this document or by header constants alone.

| Constant | Value | Meaning |
| --- | --- | --- |
| LOOM_SETUP_SERVICE_BUFFERS |8| Requires PRIVATE_SERVICE4; enables receipt geometry and pool controls. |
| LOOM_REGISTER_SERVICE_POOL |7| Create a bounded pool from fixed registration slices. |
| LOOM_REGISTER_RETURN_SERVICE_BUFFER |8| Return one exact payload lease. |
| LOOM_REGISTER_QUERY_SERVICE_POOL |9| Read one consistent pool snapshot. |
| LOOM_SERVICE_POOL |4| Kind in the existing shared service-slot table. |
| LOOM_SERVICE_POOL_MEMBERS |64| Combined per-ring member ceiling. |
| LOOM_SQE_BUFFER_SELECT |16| READ selects a member from the named pool. |
| LOOM_CQE_SERVICE_BUFFER |4| CQ slot has a nonzero payload receipt. |

Do not reuse CQ flag2: it is the existing F_NOTIF reservation. All existing
encodings stay fixed. Legacy setup/operation valid masks remain unchanged until
private runtime qualification; unsupported modes fail rather than reinterpret.

In pool mode, loom_params remains88bytes. Output _resv1[0] is receipt byte offset,
_resv1[1] its total byte size, _resv1[2] stride32; _resv1[3] and _resv0 remain0.
The receipt array has cq_entries entries, starts64-aligned after the CQ, and is
inside the page-rounded ring_size. No-pool mode leaves all these fields zero.
The kernel keeps immutable geometry separately from user-writable shared mirrors.
User geometry parsing validates every range/stride/count before pointer use.

All new control records use the existing header(size:u32, version:u16=1,
flags:u16=0), nargs1 and copy-once input rule. Unused fields, unused member tail
and output-only fields must be0. Provisional slots are invisible until successful
output copy and commit under the ring lock; copyout fault releases reservations
and refs, burning the incarnation rather than publishing a hidden pool.

| Record | Size | Fields |
| --- | --- | --- |
| pool member |24| buffer_index:u32@0, reserved:u32@4, offset:u64@8, length:u64@16 |
| pool create |1568| header@0, count:u32@8, reserved:u32@12, result:service_ref@16, members[64]@32 |
| receipt |32| pool:service_ref@0, member:u32@16, reserved:u32@20, lease:u64@24 |
| return buffer |40| header@0, receipt@8 |
| pool snapshot |64| header@0, pool:service_ref@8, members:u32@24, available:u32@28, busy:u32@32, pending:u32@36, leased:u32@40, streams:u32@44, reserved:u64[2]@48 |

Pool snapshots take only header+pool on input. Pool creation returns an ordinary
slot/incarnation. Use existing REAP_SERVICE_SLOT to remove a pool; it requires
streams==busy==pending==leased==0. Generic QUERY_SERVICE_SLOT rejects POOL;
the dedicated snapshot prevents overloading scope diagnostic fields.
The streams field counts all request references to the pool, including
single-shot pooled reads and any terminal bookkeeping still borrowing it.
Pool controls/pooled SQEs on a private ring without SERVICE_BUFFERS fail
EOPNOTSUPP; the feature cannot be enabled later by a registration call.

Pooled READ: handle_idx and _resv1[1] identify the source fid, buf_idx_or_off is
pool slot, _resv1[2] its incarnation, len is the maximum requested byte count,
offset is the service file offset, _resv1[0]=0, deadline remains _resv1[3].
Require 0<len<=every member length at submission, and apply the existing
negotiated-msize bound before Tread. BUFFER_SELECT is accepted only for READ;
MULTISHOT READ requires BUFFER_SELECT. Single-shot pooled READ is also valid.
Other combinations/reserved fields return EINVAL before any peer I/O.

The same file offset is reused on each multishot Tread, matching existing
resubmission semantics. It is intended for event fids where this is the service
contract. Regular-file sequential reading uses explicit single-shot offsets;
Loom does not invent source-type discovery or silently advance a shared offset.

## Payload state machine and publication

Each member is AVAILABLE -> BUSY -> PENDING -> LEASED -> AVAILABLE. PENDING
means a committed payload waiting for CQ delivery. Failed/zero-byte reads return
BUSY directly to AVAILABLE once no parser/copy borrows remain. There is at most
one in-flight read per multishot request. A pool may serve several requests;
round-robin ready-stream admission prevents an always-ready stream monopolizing
all returned members. Other scopes/deadlines retain the approved work quantum.

Before peer bytes, claim AVAILABLE and reserve its pending-result record plus
the request's distinct terminal obligation. Mint a ring-global64-bit nonzero
lease number before use; never wrap or reuse it. Exhaustion ends that request
with ENOSPC without admitting another shot. The counter is not user_data and
cannot be selected by the caller. Burn failed-shot numbers.

Copy the response and commit its result under the same cancellation serialization
as other private results. Positive multishot replies have MORE|SERVICE_BUFFER;
positive single-shot replies have SERVICE_BUFFER only. EOF0 ends the stream,
has no lease and no MORE. Errors/abort terminals have no lease and no MORE.
For a leased completion, result is byte count and user_data is unchanged.
Receipt entries for every non-leased CQE are explicitly zeroed before publication.

Publish CQE and receipt before the release-store to cq_tail. Both remain stable
until the corresponding cq_head acknowledgement permits their slot to be reused.
Copy BOTH before publishing cq_head. The kernel never trusts a shared receipt
for return authorization: validate a copied return record against its own pool,
member state and exact lease number. Hostile shared-head/receipt writes can
corrupt that process's observations but cannot select arbitrary kernel storage,
make a stale return release a newer lease, or bypass private-ring ownership.
Lease numbers are identities, not secrets or cryptographic capabilities: an
owner supplying another valid current receipt from its own ring is deliberately
returning that lease and assumes the same no-further-access obligation.

Return accepts LEASED only; pending results cannot be recycled by guessing a
number. A return supplies the application's explicit promise that all accesses
through that lease have ended. A matching return is atomic with re-admission;
repeat/stale returns fail ENOENT and cannot release the next lease of that member.
Bad shapes/indices fail EINVAL. A returned member wakes the bounded progress
owner using the existing register-before-recheck discipline.

No free member means wait locally, not spin and not ENOBUFS termination. No Tread
is sent until a member is reserved. Absolute deadlines still expire while waiting;
ABORT/QUERY/RETURN remain independent of SQ/CQ availability. Do not discard
server data to compensate for an empty pool. This bounds local buffering; loss
behavior in an event server remains that service's queue/sequence contract.

## Full CQ, cancellation and retirement

Pool members supply precharged pending MORE records; requests supply precharged
terminal records. No allocation in callback/abort/return, no unbounded overflow.
Stop new shots when delivery is backpressured, retaining already committed
results in their records. Preserve commit order, particularly every committed
MORE before the final completion of that same request. Cancellation cannot
overwrite/discard a prior success merely because its CQ delivery was delayed.

ABORT stops new claims and tears down the private transport. An unpublished BUSY
member returns only after parser/copy references end. PENDING/LEASED payloads
stay intact and explicitly returnable after the source scope retires. They are
owned by the ring's pool, not by a dead scope pointer. Consequently scope-local
RETIRED means no further access from that scope, not permission to recycle a
payload still held by its application. Other requests can use the member only
after RETURN; no SAK/timeout path silently acknowledges it for the consumer.

A final completion does not mean all previous payload leases were returned.
The library must track them separately. Cancellation/drop requests stop the
stream; pool destruction remains EBUSY until every lease/ref is resolved.
Strict SAK quiescence includes the client's payload users, cancellation and
local retirement; leased clipboard bytes are not declared erased by a kernel
scope terminal. Wipe only when actual local use ends, under the clipboard rules.

Whole-ring close/exec/exit detaches the consumer and follows the already approved
bounded retirement owner. It may discard queued completion delivery and reclaim
pool records only after all kernel writers/borrows stop. It cannot forcibly
unmap another holder or imply user allocation destruction: application storage
has its own references, and library handles retain it until local users finish.

## Accounting and API

Receipt bytes are cq_entries*32, within the charged ring allocation; no new ring
entry limit. At the maximum4096SQ/8192CQ geometry, the header, index, SQEs, CQEs
and receipts occupy671808 bytes before page rounding (675840 at4KiB pages).
This calculation is not an allocator measurement. Pin exact metadata sizes and
actual backing costs in the implementation ledger before activation.

All64 member records, pending queue links and terminal records are bounded and
precharged; pool slots count in the existing64 service-slot envelope. Whole
Burrow pins retain the existing exact-payer charges, including unmap/creator
exit and local retirement. Slicing does not turn a whole-object pin into a
smaller physical charge. No raising128MiB, anonymous emergency queue, per-read
stack or unaccounted heap growth. MM accounting follows as already approved.

Rust returns an owned PayloadLease retaining pool/storage ownership, with a
read-only payload view while borrowed. Reaping copies CQE+receipt as one logical
record. Return/drop cannot recycle until all safe borrows end; error paths retain
ownership until explicit closure/retirement permits release. C returns the same
receipt through an opaque lease and a documented explicit release operation.
Neither wrapper exposes legacy reap for pooled completions or assumes CQ
acknowledgement/final-stream-completion is payload acknowledgement.

## Qualification

0. Extend the explicitly re-enabled Loom model suite before runtime consumers.
   A focused provided-buffer model keeps the existing core/service models intact
   and distinguishes reply commit, CQ delivery/acknowledgement and payload return.
   Check immutable leased bytes, stale return/slot reuse, paired publication,
   bounded completion storage and local abort retirement with no peer/consumer
   return fairness premise. Each intentionally broken rule must fail its named
   safety or temporal property; retain logs and delete transient state trees.
1. Three C/kernel/Rust mirrors, ARM64 layout, independent byte vectors and
   mutation checks: constants, geometry, record sizes, every field/zero rule.
2. Actual-source state tests with two distinguishable payloads: retain A across
   B, reap CQ aggressively, return out of order, stale/double return, pool slot
   reuse, lease exhaustion, alias/overlap, pooled-vs-fixed I/O and copyout fault.
3. Full CQ and empty pool: no lost/duplicate terminal, no rearm before a member
   returns, bounded storage, ordered MORE/final, abort/timeout independent of
   consumer progress, no write after scope retirement.
4. Close/exec/exit while BUSY/PENDING/LEASED, registration races, exact refund
   and owner-image checks; safe Rust/C lease lifetime and abandoned-wrapper tests.
5. Native stalled/malformed server, distinct byte-verified shots, independent
   scope progress, existing model/mutant obligations and current sanitizer/SMP
   gates. SAK/full clipboard adoption remains a separate activation gate.

The AS-R8 raw Rust registration safety correction is part of the client gate:
make unchecked raw buffer registration explicitly unsafe, with the complete
asynchronous lifetime/exclusivity obligation, and audit existing callers. The
safe pool wrapper must consume storage ownership instead of inheriting the old
integer-VA registration API's apparent safety.

No new runtime support, tests or graphical verification is claimed by this design.
