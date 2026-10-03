# HI-1 service readiness integration

Implementation review for HI1-Q2, September 25. This is a proposed internal
arrangement within the approved userspace service; it adds no syscall, wire
operation, authority role or clipboard guarantee. The native worker is implemented and connected to the existing two-connection
session media service. Expanded interaction admission remains unimplemented.

## October 2 ownership amendment (approved)

For the activated clipboard service, the operator approved the dedicated owners
in HALCYON-INTERACTION-SEAT-PROGRESS. The session service executor replaces its
readiness-only worker and owns protocol state, clipboard mutation and admission
completion independently of rendering. The media migration is now verified by native independent-client, failure/
repost and graphical checks recorded in HALCYON-INTERACTION-STATUS. The older
readiness-only arrangement below remains historical design for PollWorker,
not the current session service owner. The UI communicates
through bounded commands/results and never waits synchronously for service work.
This does not move focus authority out of Tapestry or permit clipboard data in
Lictor. Existing nonblocking transport, identity, quota and buffer-lifetime rules
remain. No polling worker is repurposed into a parser without this explicit
consumer migration; its generic readiness-only API remains unchanged.

## The capacity constraint

The approved service reserves 32 controller, two media and four handshake
connections. Halcyon's session loop already waits on up to 32 terminal output
pipes and a Tapestry EventRing. Appending the listener and all 38 service
connections would produce 72 entries before terminal input writes; native poll
accepts at most 64. A cap applied only to the trailing writes cannot fix that.

The existing session loop already uses a bounded timeout for terminal input
pipes omitted at maximum pane count. This proposal neither introduces that
fallback nor claims to remove it. With the service represented by one wake fd,
the UI loop uses at most 34 base descriptors and retains its current write
handling. A later removal of that fallback needs its own complete readiness
accounting; it must not be disguised as a property of this change.

## Historical readiness-only arrangement (superseded for session service)

Use one bounded native worker to poll service descriptors and report readiness
through one pipe to the UI loop. Keep accept, peer checks, 9P framing, fid state,
HIN1 parsing, buffers, clipboard mutation and Tapestry admission in the UI loop.
The worker does not read application bytes, send replies or approve anything.
This narrows the earlier transport-worker suggestion: there is no second parser,
request queue or copy of a 32 KiB connection buffer to charge or synchronize.

The worker's maximum poll set is 38 connection descriptors, the listener and a
configuration/shutdown wake pipe: 40. The two directions of one connection share
one poll entry; arm READ and WRITE together when both can progress. Do not add a
second entry for an unsent reply. Service frame/byte/time budgets and round-robin
fairness remain required on the UI side, including processing complete frames
already buffered when the kernel descriptor is no longer readable.

A worker is useful only if all UI I/O is nonblocking. Accepted SrvConn server
endpoints must explicitly enable nonblocking mode: default writes block, and
byte-mode reads block too (HI1-R4). The new connection state
machine must retain a reply's sent offset on WouldBlock and arm WRITE. Both
existing media adapters now use the shared servicewire pump for this; their old
send_all policy closed on a nonpositive write. The native real-SrvConn gate
qualifies retained replies and short-write progress, and the actual session
adapter handles two routed uploads. The UI now waits on one worker notification descriptor; protocol work stays
on its original thread. Expanded persistent interaction admission remains open.

## Descriptor and notification ownership

Use a fixed slot table sized from the service limits. Each occupied slot owns
one File and a monotone registration generation. Transfer /srv descriptors into
`register_owned`: the kernel deliberately refuses aliases of devsrv Spoors and
listeners (NoSrvSpoorDup / SrvHandlesAtOrigin). Transferable sources may instead
use `register(&File)`, which duplicates before publishing. No raw descriptor is
sent for a later worker-side dup.

The UI borrows an owned registration through `with_fd` for one I/O closure. Its
exclusive mutable owner borrow prevents remove/shutdown during that closure.
The state lock is released BEFORE calling it. The worker closes only retired
slots, never live ones (including at worker failure), so the raw handle stays
pinned for the closure. Safe protocol code cannot close it; unsafe callers must
not close or reuse a saved raw fd beyond that borrow. No reference to mutable
slot memory escapes the lock. Removal marks retirement and invalidates the ID;
the worker closes after its old poll has returned. A pending result names slot
and generation, never just a raw fd. Results for retired registrations are ignored.

Each slot has desired interest, armed interest and latched readiness. After a
ready result, disarm that slot until the UI acknowledges servicing it and sets
its next interest. Otherwise a level-ready peer spins the worker while the UI
has no service credit. The UI may re-arm the same slot immediately; the next
poll sees current readiness, including data arriving between service and re-arm.
Generation exhaustion refuses new registrations rather than wrapping.

A shared mutex protects the fixed slot metadata and the two wake latches. Pipe
bytes carry notification only; bounded shared metadata carries state. Coalesce
each wake direction to one outstanding byte. The consumer must clear/drain its
latch under the same synchronization as the producer's empty-to-pending change,
then recheck shared work before sleeping. Treat a wake write/read failure as a
service failure, never as a reason to silently lose an edge. The implementation
must establish the one-byte/nonblocking invariant rather than assume that
arbitrary writes while holding a mutex cannot block. Never hold this mutex
across poll, application I/O, allocation, Tapestry RPC or thread join.

## Lifecycle and failure

Start the worker before exposing the interaction endpoint. If allocation,
descriptor duplication, pipe setup or thread startup fails, unwind all acquired
resources and keep interaction unavailable. Do not expose a partly watched
service or fall back to periodic broker polling. Existing media availability
must have an explicit failure policy in the eventual adapter. A listener close
does NOT unpost: KObj_Srv registry lifetime is the poster process. Therefore a
fatal watcher failure after posting cannot leave a live compositor with a dead
service name; without a new unpost ABI it must end the posting compositor (session
recovery must be verified separately). Starting the worker before POST avoids
publishing on ordinary constructor failure. The session caller now exits on a published startup failure or fatal service
error. Native failure and graphical recovery evidence are tracked separately
in the status note; no new unpost mechanism is proposed here.

Native libthyla-rs currently provides raw thread spawn and clear-child-tid join,
not an owning high-level thread abstraction. The adapter therefore needs an
explicit owned stack/context and startup handshake. Register the join word
before announcing startup. On shutdown, stop new registrations, wake the
worker, wait for kernel-confirmed exit, then release stack/context/pipes. A
join timeout is not permission to free live memory. Do not copy kaua-term's
process-lifetime leaked pump context as a reusable service lifecycle.

The worker can never outlive the compositor process. Unexpected worker exit
must wake the UI and make the endpoint unavailable; a pending operation cannot
be treated as approved. Logout and trusted-seat takeover still cancel pending
normal-seat work through the ordered UI/Tapestry path. No worker readiness bit
is an admission result. Tapestry's positive focus/terminal check remains the
admission point specified in the main interaction and PTY contracts.

## Evidence required before enabling the expanded service

- Assert worker/UI poll counts and the complete allocation ledger: connection
  buffers remain under section 8's 7.375 MiB ceiling; metadata, descriptor refs,
  pipes, worker stack and thread context are separately counted and bounded.
- Deterministically exercise ready-before-arm, ready-during-rearm, wake/drain
  races, simultaneous two-way updates and last-slot removal/reuse. Show that a
  permanently ready peer cannot spin a disarmed slot or starve another slot.
- Cover short writes and WouldBlock, multiple buffered frames, full replies,
  disconnect while disarmed, cancellation, handshake expiry and full pools.
- Inject allocation/dup/spawn failures at every acquisition boundary and verify
  rollback. Prove orderly shutdown joins before freeing borrowed memory; retain
  diagnostics and live storage on any ambiguous join failure.
- Run a native guest test with real descriptors, all connection slots and a
  quiet compositor. Check both progress and absence of periodic service wakes.
  Pure state-machine tests alone cannot establish actual wake/close behavior.

Aux cleared TC-1a at 1cc9a300; Main is qualifying that merge. Respect the
remaining TC-1b file reservations recorded in the status. The host binding
announcement and its decoder must still land together on the reconciled base.

## Standalone worker checkpoint

libthyla-rs poll_worker supplies the proposed readiness-only mechanism. It
supports at most 63 entries; the Halcyon adapter will request 39 (38 connections
plus listener), for a 40-entry worker poll. Native validation and named negative
controls are recorded in HALCYON-INTERACTION-STATUS.md. This is not activation:
complete the remaining admission, failure and memory qualification before
increasing the connection count. No existing terminal-write fallback
or clipboard admission behavior has changed.

## Existing session media integration (September 25)

PanePlaceServer transfers the listener and accepted endpoints into PollWorker.
Only the wake descriptor reaches the UI poll vector. The listener disarms when
full; `free_slots` counts retired entries as occupied until their old poll has
returned. Reclamation itself notifies the owner, so acceptance resumes without
a timer or a transient accept-and-drop. Complete buffered work stays UI-runnable
with its watch disarmed; partial input and blocked replies re-arm actual I/O.

The image residual additionally reserves 72 KiB for the worker: 64 KiB stack,
4 KiB guard and a conservative 4 KiB context. This is not the complete expanded
38-connection ledger: kernel allocations and per-connection metadata still need
explicit accounting before that admission limit is enabled.

The opt-in `poll-worker-test` feature supplies single-owner bounded rendezvous
outside the worker mutex, and one-shot acquisition/failure injection. The probe
enables it through `readiness-qualification`; neither feature is default.
Ordinary builds contain neither control state nor qualification commands. Tests
must opt in explicitly and use a matching image; default interactive sweeps skip
these two fixture-dependent gates with status 77.

## Dedicated native adapter ledger (October 3)

The readiness-only arrangement above is historical. The qualification service owner
polls at most 42 entries: 38 connections, listener, control wake, HSC and HIA.
There is no per-connection worker or ring. Each of its two control channels has
one 4-entry SQPOLL ring (one page at the current geometry) and one page-rounded
registered buffer. Four worker pipe FDs and a pinned reservation are additional
fixed resources. The UI has only the service notice fd; route and image results
cross a bounded mailbox.

Userspace payload/protocol ceiling remains 7.375 MiB. Metadata is reserved
separately: 38 * 8 KiB connection upper bounds plus 48 KiB Link plus 16 KiB Shared =
368 KiB. A 512 KiB allowance covers those objects, the two ring/buffer mapping pairs
and remaining fixed vectors/context/allocation slack. The worker's 128 KiB stack
and 4 KiB guard are additional. Const assertions enforce native object ceilings;
the qualification image residual deducts the entire reserve before offering raster capacity.
Output buffers grow lazily; 8 KiB msize leaves room for 16 KiB fragmented records
inside the aggregate 32 KiB input budget. Retained capacities, not just lengths,
are charged. Media still has exactly two raster slots.

Kernel costs remain separate: each accepted default service connection costs
one credit and 128 KiB of ring storage; 38 peers alone use 4.75 MiB of such rings.
Attached kernel 9P clients and receive/RPC/fid storage add costs described in
HALCYON-INTERACTION-CONNECTION-BUDGET. The complete pressure ledger, native
all-slot and multi-session application load are still HI1-R24, not implied by
the two-process byte/SAK test or by the userspace reservation calculation.
