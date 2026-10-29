# Progress prerequisite for the approved clipboard / SAK barrier

October 2, 2026. **Dedicated control owners approved by the operator, October 2.**
Option A in HALCYON-INTERACTION-SEAT-REVIEW is approved. This document does not
reopen strict cancellation or its bounded timeout. It identifies the execution
ownership change needed to implement that policy without introducing a circular
wait in an otherwise healthy session.

## Verified dependency cycle

The October 1 review missed a dependency of the proposed cancellation pump.
At baseline 981b89caa:

1. `Surface::submit_present` in `usr/lib/libtapestry/src/lib.rs` calls synchronous
   `t_write`. Its Rwrite is the surface-slot recycle gate. Halcyon's UI owner
   cannot do other work until this returns. The source explicitly documents this
   and no longer uses an asynchronous Loom WRITE for presentation.
2. `Conn::h_write` dispatches presentation through Comp inside that request.
   The compositor's synchronous GPU facade calls `rpc_client::Client::call`,
   whose `read_exact` blocks in `t_read` until Lictor returns its result.
3. In `usr/lictor/src/main.rs`, a non-normal seat parks ordinary GPU requests.
   Only input queries/drain and SeatState may run. The normal request can have
   been queued immediately before the physical chord but not executed yet.
4. Existing Lictor can complete the trusted episode independently, restore
   NORMAL and then release the parked request. Adding the proposed main-loop
   aggregate ACK prevents that path: Lictor waits for Tapestry, Tapestry waits
   for Lictor, and Halcyon may also be waiting for Tapestry.
5. The existing five-second deadline eventually breaks the wait by failing the
   episode. It does not make normal concurrent rendering a valid reason for
   SAK refusal. The same dependency exists during setup and synchronous control
   operations; converting just the steady-state present call is insufficient.

This was source-level reachability at the design checkpoint. The October 2
implementation and native forced parking evidence are now recorded in
HALCYON-INTERACTION-STATUS; old graphical runs are not relabelled as new tests. Evidence in `work/oct2-hi-seat/source-path.json` pins
the inspected files and source baseline.

## Unsafe shortcuts excluded

Do not acknowledge before broker cancellation, reinterpret timeout as success,
let normal presentation reach the trusted display, kill a renderer, or return a
fabricated successful GPU/present result. The Rwrite owns a real slot lifetime;
completing it early can allow the producer to overwrite a slot still in use.
A second notification queue on the same blocked owner does not break the cycle.

## 1. Independent userspace control owners (approved)

Give Tapestry's cancellation coordinator an independently scheduled, bounded
control loop and give the session clipboard service one independently scheduled
owner for its protocol, pending actions and payloads. Keep their state out of
blocking graphics calls. This changes the earlier UI-owner/readiness-only
arrangement in HALCYON-INTERACTION-READINESS; the amendment is ratified here.

- One additional coordinator thread in the existing Tapestry process. It owns
  the fixed participant table and a separate bounded 9P cancellation lane;
  graphics and focus remain with the existing compositor owner. A kernel-stamped
  peer identity plus registration by the existing declared renderer connection
  ties the lane to the exact session incarnation. A route string is not authority.
- One clipboard/service executor per Halcyon process, replacing the current
  readiness-only service worker rather than adding one worker per client. It
  owns parsing, fid state, broker state and outgoing application replies.
  The UI sends bounded commands and consumes bounded results; it never lends
  mutable clipboard or transport buffers across threads.
- Normal focus admission still goes through the existing authenticated HIA1
  path. The executor may cancel its local pending action while the HIA1
  exchange remains in flight; it retains that exchange's buffer until actual
  completion or safe teardown. A late receipt cannot publish after cancellation.
  Cancellation control uses a separate connection, not the blocked normal RPC.
- Participant registration and quiescence share a short state lock. Entering
  quiescence freezes the set; no normal register/open path can enable a broker
  after the snapshot. Never hold this lock across GPU, 9P, allocation or join.
  A live participant's control EOF remains an unsatisfied obligation.
- The lane adds a resident route and bounded connection/handle charges, to be
  recorded in the D7 registry contract before use. It does not introduce a new
  kernel syscall, role, transferable credential or hardware permission. Existing
  session/global connection quotas still apply; capacity failure refuses setup.
- Lictor continues to accept aggregate control from its exact designated
  compositor process only. Secret input and hardware ownership are unchanged.
  A coordinator/executor that genuinely stalls still triggers the approved
  five-second timeout, so strict cancellation is never weakened for availability.

The exact lane encoding and allocation ledger follow in the scripture amendment
before implementation. The participant table remains bounded by compositor
capacity; queues must be fixed-capacity and refuse overflow without dropping
revocations. Shutdown must account for outstanding I/O before releasing memory.

This retains the useful separation illustrated by Genode's userspace clipboard
component (the primary-source reference is in the original review). It is not a
claim that Genode supplies this exact SAK protocol. It is a proposed local
ownership correction, not a second clipboard or a generic IPC framework.

## 2. Make the graphics path resumable first (not selected)

Keep clipboard/protocol ownership on the UI threads as originally specified.
Replace synchronous presentation and compositor-to-Lictor operations with
resumable operations so cancellation/control remains serviceable while a normal
request is parked. This preserves the original owner model and avoids a separate
control executor, but is a broader rendering/transport refactor: slot recycle,
weave lifetime, all GPU call continuations, setup/teardown, error propagation,
and client control calls must be covered. Changing only Surface::present or
using a helper thread that the owner immediately joins does not suffice.

This is a sound longer-term architecture. It carries a much larger regression
surface than moving the still-unactivated clipboard service to a dedicated
owner. Neither alternative may regress the approved cancellation guarantee.

## Verification required for either refinement

In addition to the original review's acceptance set, park a real normal GPU
request before processing the physical chord, and independently park a real
Halcyon present. Show that cancellation completes and trusted input opens
without waiting for those normal responses. Confirm the producer cannot reuse
its presentation slot early. Then restore and prove the original rendering
request completes without corruption or loss of its reply. Also exercise a
truly stalled participant and observe the approved bounded refusal.

Keep exact logs and screenshots. Source reasoning alone does not qualify this
runtime ordering. No new SMP/sanitizer gate is required under the October 2
operator waiver, but focused native concurrency and lifetime checks remain owed.

## Encoding and initial resource ledger

The independent resident route is `tapestry-interaction`, owned by the same
Tapestry process. Add it explicitly to login's fixed D7 route manifest; user
posts cannot shadow it. It grants no access until the connecting process is
matched by kernel stripes to a reservation made on its declared renderer
connection. Reserve by that connection before enabling the clipboard endpoint.
A reservation is a monotone nonzero u64 incarnation, not a bearer credential.
Control reconnect cannot inherit a dead connection's incarnation.

The coordinator holds eight participant slots (the compositor's current total
connection bound), plus at most two unbound handshakes. The separate lane uses
one session-charged connection per broker, subject to the existing D7 16/48/64
limits. The Lictor coordinator connection uses one existing boot-domain slot
and one of Lictor's 16 connection slots. Setup refusal keeps the endpoint off.
No registry, connection, pane or clipboard payload quota is raised.

Internal HSC1 records are fixed-size little-endian, version 1. Requests are
40 bytes: magic at0, u16 version at4, u16 operation at6, nonzero u64 request ID
at8, reservation at16, seat generation at24 and observed revision at32. Operations
1 Join, 2 State, 3 Cancelled, 4 Retire; unknown values or trailing bytes fail.
Join binds the connecting stripes to the reserved incarnation in normal phase;
State observes or waits for a changed revision (at most 250 ms; an unchanged
observation lets an idle executor retire without waiting for the next episode). Cancelled acknowledges the exact
quiescing generation and revision only after local cancellation and retirement
of application writes. Retire reports orderly disabled service, not a transport
EOF. Repeated exact transactions are idempotent; request ID reuse with changed
content, decreasing IDs and exhaustion fail closed.

Replies are 56 bytes: the same prefix/op/request, reservation at16, generation
at24, revision at32, u32 phase at40, u32 flags at44, reserved-zero u64 at48.
Only flag bit0 (participant enabled) is defined. Kernel seat phase values are
mirrored unchanged; an unavailable/broken coordinator never reports normal.
Revision is monotone, and every phase, registration or retirement transition
advances it. A cancellation obligation is identified by the frozen generation
and that participant's suspension revision; unrelated acknowledgements do not
change its identity. Wrap/exhaustion disables admission, never reuses identity.

Normal-connection reservation/control encoding is separate HSR1 with the same
40/56-byte envelope, version 1: operation1 Reserve, operation2 Withdraw.
Reserve takes zero reservation/generation/revision, authenticated peer identity
and declared connection from the server. Withdraw requires an exact reserved
incarnation that has never enabled or has completed orderly Retire. The main
loop and coordinator serialize these short table transitions; neither holds the
table lock across protocol I/O. It cannot withdraw a live participant to skip ACK.

The aggregate to Lictor extends its existing typed broker request with opcode66 (65 remains the shipped pointer Cursor),
SeatQuiesced followed by u64 generation. Only the currently designated compositor
process can use it. Lictor accepts it only in QUIESCING for its exact current
generation, and does not reuse it for another episode. Kernel ACK still requires
private scanout, retired normal hardware work and released physical keys.

Each new coordinator/executor thread reserves a 128-KiB stack plus one 4-KiB
guard, with fallible construction and kernel-confirmed exit before reclamation.
The Halcyon executor replaces its existing PollWorker rather than stacking a
second service worker. The UI command side is a fixed desired-route table of 32 metadata records,
each below 128 bytes; repeated updates coalesce. Results use two moved-image
slots, below the approved 32-record ceiling. Payloads stay charged to existing
service limits. Capacity refuses new work; revocation modifies durable table
state and cannot be dropped. Nonblocking pipe hints drain in batches of 64;
EAGAIN means a wake is already pending. Completed images retain their exact
route token and publish before client success.
Control wire buffers are capped at 4 KiB input plus 4 KiB output per lane.
At ten coordinator connections that is at most 80 KiB, excluding fixed fid,
participant and thread metadata (separately measured before activation).

All lifecycle and buffer charges, including paused exchanges and retired peers,
remain in the activation ledger until actually reclaimed. No early reply permits
surface-slot reuse or normal display admission. Native tests must qualify the
ownership transfer and independent progress, not only the wire codec.


## As-built qualification limits

The independent worker uses four pipe FDs and a 128-KiB guarded stack (132 KiB
reservation). Each Halcyon executor additionally owns two four-entry SQPOLL
rings (HIA/HSC), registered fixed buffers of 120/96 bytes, pinned ctl files and
one reserved HSR fid. Kernel mappings, pipe pages, retained connections and
allocator overhead remain owed in the complete activation ledger; the 64-KiB
service metadata reserve in Halcyon's image-budget calculation is conservative
headroom, not a measured total. Coordinator metadata is fixed for eight members,
ten connection records and four fids per connection; wire storage is bounded
at 80 KiB. The coordinator's 10ms status loop adds periodic work; total idle CPU
cost remains unmeasured and belongs with the existing Lictor idle-cost item.

Native qualification forces a real ordinary RPC after observing QUIESCING and
proves it parked before ACK. It also proves the application's present remains
pending through EXCLUSIVE and returns after restoration. This closes the
circular-wait progress dependency without changing the rendering completion
contract; a separate pre-chord queued-request timing experiment was not run.
The unfinished app adapter must cancel queued/partial clipboard replies before
calling Cancelled; an empty broker in this checkpoint is not evidence of that
future adapter's correctness.
