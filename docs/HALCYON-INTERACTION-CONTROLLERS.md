# Controller lifecycle adapter

Implementation detail of the approved HI-1 ownership contract; no new wire,
kernel authority or admission point. The service executor owns a fixed table of
32 terminal controller entries. A pane token locates an already authenticated
host route and is never a credential. Each route includes its local incarnation,
leaf, binding and foreground epoch. Direct graphical ownership is a later adapter
to the existing surface contract, not a terminal-route shortcut.

Preparation requires a live kernel peer snapshot (connection incarnation,
process stripes and principal), the session principal, an authenticated host route,
and the current normal seat generation. HIN1 Bind supplies the application-owned
context ID and epoch; the adapter checks their nonzero form and binds them to
that route and the freshly sampled peer. They are names, never credentials.
Tapestry retains the full published tuple and checks it on subsequent admission.
This follows the application-state ownership in the root design and the HIN1
Bind codec; no host-to-application context allocation channel is introduced. An occupied
leaf or binding is refused until explicitly retired. Preparation burns a fresh
monotone controller generation even if its eventual publication fails.

The entry starts pending. Its HIA Publish request is sent through the existing
serialized admission channel using that channel owner's increasing request ID.
Only the exact successful Publish receipt, matching foreground and seat, plus
a fresh matching live kernel peer snapshot, makes it active. Wrong receipts do
not consume another pending publication; an exact failed publication retires
its entry. This table does not supply kernel peer snapshots or perform HIA I/O.
The caller must drain in-flight HIA work before channel reuse, even after a
publication has been cancelled locally. One request sequencer must cover all
Bind/Publish/Check/Unbind operations when the channel adapter is connected.

Mode reports require the complete scope and exact peer incarnation, a strictly
increasing nonzero sequence and at most 64 bytes of control-free UTF-8 context.
Labels are stored inline. Invalid/stale reports leave the last record intact.
Unknown or pending controllers have no reported mode; presentation derives APP
or its own transcript state rather than guessing INS/NOR. Presentation chooses
an exact host route; peer reports never choose the focused tile.

Disconnect, route removal/replacement, terminal foreground/subject retirement and seat
loss remove pending and active entries. Retirement yields the exact old Owner
to the broker cancellation callback before a replacement can register. A stale
route-removal event cannot retire a replacement incarnation. Terminal snapshots
must include both epoch and subject: ACK can change nomination at the same epoch.
An unacknowledged/dead nomination supplies subject zero. Seat restoration
starts empty, never revives old scopes, and preserves the generation counter.
Focus-only loss is different: preserve the registration/mode and use the broker's
ordered focus-loss epoch handling; do not treat a return to focus as registration.

The table is allocation-free with a compile-time 16 KiB metadata ceiling.
That bound is only this table, not a claim about the total session allocation.
Public dispatch remains disabled until authenticated route/context delivery,
ordered focus and terminal retirement, request serialization, pending/partial
reply cancellation, clients and the complete activation ledger are connected
and exercised together.

## Shared execution owner

The executor's `Interaction` holds the controller table and Broker together.
The Broker's monotone request sequence covers both clipboard CHECK and control
Bind/Publish/Unbind; no second sequence is started for publication. A single
in-flight slot stays occupied until its exact HIA completion is consumed, even
when local cancellation, disconnect, timeout or SAK has already invalidated the
application operation. This is separate from the transport's borrowed buffer
lifetime: freeing one does not imply the other has completed.

Trusted ordered focus/terminal/route/peer events enter this owner before a
completion from the same executor pass. Terminal retirement synchronously
calls Broker::drop_owner. Focus loss calls Broker::lose_focus and retains the
registration/mode. Seat changes retire every controller and cancel the Broker
before HSC acknowledgement. A late completion can drain the slot but cannot
restore cancelled authority. Output is returned as values to the protocol owner;
this does not itself send a reply or close a partial application frame.

This composition implements the already-approved serialization contract. It
does not invent a lossless feed from existing TEV_FOCUS/TEV_LAYOUT events:
those are intentionally coalesced. Production delivery still needs the distinct
ordered control feed and receipt/event ordering described in the status note.

The session's HSC Link now constructs this owner with its already kernel-read
principal, and applies every join/cancel/stop seat transition to it. Bind stores
the local route incarnation even before there is a controller; route removal
invalidates that receipt. Host Unbind retires local controllers/transfers before
sending the request, including when the transport later refuses it. Zero focus
epochs are malformed for every receipt, including CHECK (layout epochs start
at one). An 18 KiB compile-time ceiling covers combined inline metadata; payload
allocation remains the Broker's separate bounded store.

The application-to-HIA dispatch adapter is still absent. Its control/publication
timeout must close or drain the channel and deliver a terminal error to this
owner. The shared deadline core below covers every request kind; its runtime
dispatch and teardown adapter must still be connected.
No public activation or total-resource qualification follows from instantiation.

## Admission deadlines and drained completions

Every shared-owner request carries the existing 30-second admission allowance,
measured in monotonic milliseconds. Bind and Publish use the same allowance as
CHECK. A regressed clock fails the request; a start too near u64 exhaustion to
represent the deadline is refused before publication or payload mutation.
Expiry returns one terminal result and retires a provisional controller before
any late receipt can activate it. It never releases the transport slot. An exact
late completion drains that slot without emitting a second result. Unmatched
completions cannot drain it. Completion itself checks the deadline, so a delayed
executor pass cannot admit work by processing its reply before its timer.

The transport adapter must close/join or drain the actual channel after expiry.
A transport-closed notification may release the slot only after outstanding I/O
has ceased; it permanently disables this owner and retires all registrations and
transfers. A fresh service instance, HSC join and registrations are required for
recovery. This is userspace lifetime bookkeeping, not a new authority or ABI.
Public dispatch and the adapter remain gated until connected and qualified.

## Deferred application transport replies

The bounded 9P stream pump permits one parked reply per connection while
continuing to parse other requests, including Tflush. The protocol owner keeps
the pending tag/fid and semantic result. A nonzero, increasing local ticket
identifies each park; client tag reuse is not a ticket. A second park must be
refused by the protocol while the first exists. The transport rejects accidental
double parking. Immediate replies retain their existing single-buffer ordering.

Resumption checks the exact park ticket and an empty output slot before invoking
the reply builder. Busy, cancelled or stale resumptions cannot mutate retained
output. Tflush can cancel the exact park and queue its ordinary immediate reply.
An empty result is legal only when explicitly parked, never an implicit success.
No new wire format, allocation pool or reply queue is introduced.

For seat retirement, discard buffered requests and wholly unsent replies before
acknowledging. If any byte of the current frame has been written, permanently
close that connection rather than replace its suffix or reuse its framing.
The protocol owner must also discard its retained semantic state and buffers;
stream cancellation alone is not the aggregate SAK barrier. Ticket generations
survive cancellation. Completed replies already delivered are not recalled.

## Route mailbox lifetimes

The UI's fixed 32-slot desired route table allocates a nonzero, never-reused
incarnation when it inserts a pane. Re-registering the same live token/leaf is
idempotent; changing either half of an occupied pair is refused. Removal never
needs a spare slot and never resets the allocator. Exhaustion refuses insertion.
A copied desired snapshot can coalesce remove/recreate safely: the executor
retires each old incarnation absent from the new snapshot before using it.

The media adapter pins the route when a fid first walks into a token directory.
A dead fid cannot walk, clone, open, read, write or stat its replacement, even
when token and leaf are reused. Clunk and flush remain possible. Queued media
results carry the same incarnation and are checked both at publication and UI
drain. This does not change the one-principal media authority policy.
