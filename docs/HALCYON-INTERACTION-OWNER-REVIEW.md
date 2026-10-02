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
