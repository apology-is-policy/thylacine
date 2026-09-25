# HI-1 service readiness integration

Implementation review for HI1-Q2, September 25. This is a proposed internal
arrangement within the approved userspace service; it adds no syscall, wire
operation, authority role or clipboard guarantee. The standalone native worker is implemented and guest-tested; the expanded
service and Halcyon event-loop connection remain unimplemented.

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

## Prefer readiness aggregation over a second protocol executor

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
adapter handles two routed uploads. Expanded persistent interaction admission
and the worker-to-compositor connection are still activation work.

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
publishing on ordinary constructor failure. This activation policy still needs
a native failure test; no new unpost mechanism is proposed here.

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

Keep Aux's uncleared TC-1a wire/lib/tile files untouched while developing this
adapter. The host binding announcement and its decoder still land together
once his exact cleared base is available.

## Standalone worker checkpoint

libthyla-rs poll_worker supplies the proposed readiness-only mechanism. It
supports at most 63 entries; the Halcyon adapter will request 39 (38 connections
plus listener), for a 40-entry worker poll. Native validation and named negative
controls are recorded in HALCYON-INTERACTION-STATUS.md. This is not activation:
complete the remaining failure/interleaving tests and live service integration
before increasing the connection count. No existing terminal-write fallback
or clipboard admission behavior has changed.
