# HI-1 service readiness integration

Implementation review for HI1-Q2, September 25. This is a proposed internal
arrangement within the approved userspace service; it adds no syscall, wire
operation, authority role or clipboard guarantee. It is not implemented yet.

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

A worker is useful only if all UI I/O is nonblocking. The current SrvConn server
endpoint supplies nonblocking reads and short writes. The new connection state
machine must retain a reply's sent offset on WouldBlock and arm WRITE. It must
not retain paneplace's send_all policy, which closes on a nonpositive write,
for the new persistent interaction connections. This is a prerequisite to
activation, not evidence that backpressure already works.

## Descriptor and notification ownership

Use a fixed slot table sized from the service limits. Each occupied slot has a
monotone registration generation and an owned duplicate of the descriptor to
poll. Acquire that duplicate before handing the slot to the worker: copying a
raw fd for a later worker-side dup permits close/reuse to select another object.
The UI keeps its own I/O descriptor. Remove the watch before releasing the
connection's final UI state. A pending result names slot and generation, never
just the raw fd; results from removed registrations are discarded.

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
must have an explicit failure policy in the eventual adapter.

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
