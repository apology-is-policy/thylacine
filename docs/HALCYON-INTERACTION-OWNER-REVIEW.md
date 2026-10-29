# Shared interaction owner self-review

Author review only; no independent audit is claimed.

R1: a pending Bind precedes controller registration. Controller-only retirement
would miss it, and a late success could target a reused leaf. Flight now retains
the exact local route incarnation; matching route removal invalidates it without
freeing transport storage. The old-incarnation event schedule and leaf-only
mutant distinguish both cases.

R2: CHECK accepted focus epoch zero, unlike Publish. Tapestry Layout starts its
epoch at one. Broker now refuses zero and releases the pending payload, tested
through the composed owner and an intended zero-focus mutation.

R3: host Unbind must retire local transfers before its asynchronous result. A
transport refusal cannot revive authority the host has removed. The explicit
host-unbind test and omitted-retirement mutant exercise that boundary.

R4: the native probe imports actual service files into its own crate. Updating
session_seat to Interaction required importing controllers and interaction there
too. The first full image build caught E0432; its log is preserved. Guest Halcyon
compilation alone did not cover that source-sharing adapter.

R5 harness: a bare drop_owner string matched three sites. The mutation driver
refused to run the ambiguous replacement. It now matches only retire's return
expression; no compile failure is accepted as a successful mutant.

Review checks: there is one mutable owner, no locks or borrowed I/O added. One
Broker request counter covers control and clipboard actions; failures may burn
an ID but never reuse it. Exact Request matching protects the transport slot.
Cancellation removes authority/payload promptly but leaves the slot busy until
its real completion. SAK also invalidates control-only flights, including a seat
round trip to the same value. Every entry retirement removes its mode before a
replacement can publish. Focus loss alone retains mode/registration and preserves
the earliest Get/Commit boundary. The 32-entry table and payload bounds remain;
combined inline metadata is at most 18 KiB inside the guarded 128 KiB service
stack. Stack peak and aggregate session allocation are not measured here.

Host evidence: 499 Halcyon tests, 49 combined actual-source schedules including
12 owner cases, nine intended owner mutations, and the unchanged 37-test/eight
controller mutation gate pass. Guest no_std binary check passes. Evidence is
in work/oct2-hi-owner. The corrected full build, CPU1 boot 1830/1830 and
physical F10 graphical scenario pass (89.07s). Prompt/restored screenshots were
inspected at 1280x800. This runtime has no application controllers because public
dispatch stays off; native populated-controller qualification is still owed.

Open integration: application HIA dispatch, fresh peer/context delivery, ordered
terminal/focus events before completion, control/publication deadline teardown,
partial reply cancellation, resource ledger and native clients. HSC now owns
Interaction but the HIA application lane is still idle. This does not close the
public endpoint activation seam. October 2 SMP/50-boot/sanitizer waiver applies.

## Initial normal seat and bounded control completion

R6 / HI1-R20: the codec correction did not remove two owner-layer zero filters.
The composed regression fails on the original owner before publication; separate
mutants restore each filter. A full in-memory registration, copy, commit, paste,
unchanged sample, revocation and same-generation rejoin proves initial zero is
usable while revoked scopes remain dead. None alone means no normal membership.

Control and Publish now use the same 30-second allowance as CHECK. The owner
checks the clock on both timer delivery and exact completion. Expiry delivers
one failure and removes provisional registration, retaining the actual flight.
An exact late completion drains without a second result; a wrong Request leaves
it busy. Starting near numeric exhaustion is refused before any owner mutation.
A confirmed closed channel disables this owner permanently, even after a later
seat sample. Only the future runtime adapter may attest closure after joining
outstanding I/O; this pure core neither drops a Ring nor proves native teardown.

Self-review checked provisional-entry cleanup, monotone IDs, all three request
kinds, cancellation before timeout, clock regression and boundary equality,
wrong/duplicate completion, payload and mode retirement, and closed-owner reuse.
No locks, allocation, wire changes or new authority are added. Inline metadata
still satisfies its 18 KiB ceiling. Runtime app dispatch remains off; guest
compilation and native SAK regression do not substitute for populated two-client
or partial-reply cancellation qualification. Measured evidence is recorded in
HALCYON-INTERACTION-STATUS and work/oct2-hi-initial-seat.

## Deferred reply transport and cancellation review

The protocol chooses an explicit immediate Reply, Park(ticket), or
Cancel(ticket)+reply. Exactly one park is permitted per connection and its ticket
must increase. Resumption checks a matching ticket and empty output slot before
calling the builder, so even a bad callback cannot mutate a retained reply on a
stale/busy resume. No additional reply allocation or queue exists; metadata still
fits 64 bytes and frame/byte/time budgets apply while parking and flushing.

Cancellation clears buffered input and the unsent reply. A partly sent frame
poisons the connection permanently; the caller must close its actual endpoint.
Already fully delivered frames are not recalled. The handler must retire its
own request/cache/controller state before an HSC acknowledgement, and this
transport is not itself an authority check. Existing media handlers always
return immediate Reply and retain their publication-before-Rwrite behavior.

The owner now exposes drain_required: cancelled CHECK expiry can return no new
application result while retaining transport work. Tests pin that durable flag
and its removal on exact drain. This was identified while designing the runtime
adapter, before enabling dispatch. Six transport mutations and the added owner
mutation cover overwritten busy output, stale tickets, ticket reuse, lost
cancellation, partial-frame reuse and buffered-input survival. Real SrvConn
schedules additionally exercise park/progress/resume/flush/unsent cancellation;
partial-write cancellation is controlled host evidence. No independent audit or
complete clipboard/SAK barrier qualification is claimed.

## Route lifetime handoff review

HI1-R21: a live token could be retargeted by Routes::insert; the red witness in
work/oct2-hi-routes/red.log reproduces it. A coalesced remove/reinsert of the
same token/leaf also had no distinct identity. The fixed table now assigns
non-reused incarnations, rejects occupied token/leaf changes, permits idempotent
registration, and allows removal even when capacity or the allocator is spent.

Actual media fids pin that incarnation on the first token walk. Revalidation
covers clone, parent walk, open, read, write and getattr, including a partially
uploaded image. Clunk remains usable. Complete images are checked before mailbox
publication and before UI delivery. UI-driven route mutation cannot silently
retarget a retained result. The same copied-table comparison retires old broker
owners before the service consumes its next admission record. The table is
compile-time bounded to 2048 bytes; this is not a full service-resource ledger.

The protocol test compiles production parsers, route code, accumulator and
handlers. Six mutations cover live retargeting, duplicate leaves, reused
incarnations, stale-fid admission, lost snapshot retirement and name-only
completion checks. The native extension uses real kernel 9P fids, partial writes,
a queued completion and a newly opened replacement. Results are in the status
note. No independent review or full clipboard activation is claimed.

The context-source wording is clarified to match root-design section 2 and HIN1
Bind: apps name their fields; authenticated host routes and fresh kernel peers
establish ownership. Context IDs cannot establish identity. Both current UI
registration and preopened ordered control use the declared EventRing session,
so a different compositor connection is not silently substituted. Moving Bind
into the dedicated owner and delivering authenticated host metadata remain part
of the next dispatch adapter; the current public endpoint stays disabled.
