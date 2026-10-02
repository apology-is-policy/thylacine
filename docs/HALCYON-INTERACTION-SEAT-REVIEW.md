# Clipboard cancellation at trusted-seat takeover

Status: **A approved by the operator, October 2, 2026** ("Let's do A.").
Source investigation baseline `fda6de374`. Strict cancellation before trusted
input and the existing five-second timeout are ratified. B is rejected.
Implementation and live endpoint activation are not established by approval.
See HALCYON-INTERACTION-SEAT-PROGRESS.md for the blocking presentation dependency
found while tracing the implementation.

## Finding

HALCYON-INTERACTION section 9 requires pending clipboard transfers to be
cancelled when SAK owns the seat. HALCYON-INTERACTION-PTY-ABI says cancellation
precedes trusted input. The current display exclusion path does not establish
that ordering for Halcyon's separate clipboard owner.

Verified source path:

- `usr/lictor/src/backend/seat.rs`, `Seat::step`: after normal GPU work retires,
  private scanout is active and physical keys are released, Lictor submits ACK.
- `kernel/proc.c`, `proc_console_sak_from`: ACK checks the exact seat service,
  generation, quiescing phase, deadline and held keys. There is no clipboard
  participant acknowledgement.
- `usr/tapestryd/src/main.rs`: a non-normal seat suspends local interaction
  state and skips normal connection servicing. That does not retract receipts
  already written to Halcyon's transport.
- `usr/halcyond/src/clipbroker.rs`, `Broker::seat`: correctly cancels when
  called. `Broker::complete` cannot discover an external transition whose
  notification has not reached its owner.

A permitted schedule is CHECK accepted in generation G; its success reply is
queued; physical SAK advances to the trusted phase; Halcyon consumes the old
reply before seeing a seat notification and publishes the pending copy.
Polling immediately before publication has the same check/use gap. Servicing
ready notifications before ready replies is necessary, but cannot order a
notification that has not arrived.

This is a missing integration guarantee, not an activated clipboard exploit or
a demonstrated Imperium-key leak. The public clipboard endpoint remains off.
Lictor's secret input path never routes key bytes through this broker.

`work/oct1-hi-seat-review/` contains two controlled schedules compiled against
unchanged production store/broker sources: delayed notification permits the old
publication; cancellation before completion prevents it. Both assertions pass.
These are source-level schedules, not a live kernel/SAK or graphical test.

## Prior art and fit

[Plan 9 rio's snarf file](https://9p.io/magic/man2html/4/rio) exposes clipboard
text through read/write file operations; recursive rio shares its parent's
snarf. This fits our userspace file protocol, but the manual specifies no
cross-service trusted-seat cancellation barrier.

[Genode's clipboard component](https://www.genode.org/documentation/release-notes/15.11)
mediates report/ROM data between configured GUI domains, including directional
flow policy. It supports keeping clipboard policy separate from the display
and hardware service. It does not establish Thylacine's specific pending-work
cancellation guarantee.

[Fuchsia focus](https://fuchsia.dev/fuchsia-src/concepts/ui/input/focus) separates
view focus notifications from product-specific guarantees such as lockscreen
keyboard exclusion. The relevant lesson is to name exactly which operation is
excluded; keyboard exclusion alone is not a cross-process clipboard barrier.
These primary sources were checked October 1. The following design is our
inference for the verified Thylacine architecture, not an attributed upstream
implementation.

## A. Preserve strict cancellation with a userspace quiescence barrier (approved)

Keep the existing kernel seat phases, process roles and five-second quiescence
deadline. Extend the authenticated Lictor/Tapestry protocol: Lictor's final ACK
requires both its existing hardware conditions and an exact-generation normal
interaction quiescence acknowledgement from its kernel-designated Tapestry
client. Lictor knows only that aggregate state; no clipboard text, app identity
parser or clipboard storage moves into Lictor or the kernel.

Tapestry closes new interaction admission and sends a generation-bound suspend
to every enabled session broker, including hidden sessions. Brokers participate
before enabling their endpoint, through the existing declared renderer
connection. The participant set is bounded by the existing compositor connection
limit; duplicate registrations do not allocate another participant. No broker
can join during quiescence. The October 2 progress amendment moves cancellation
control to a separately serviced lane, bound through the declared connection. Even zero participants requires Tapestry's aggregate
acknowledgement, so a delayed registration cannot race the snapshot.

Each broker's service owner closes admission (the October 2 progress amendment
places this owner on a dedicated executor), calls `Broker::seat(None)`, discards
unsent application success/data replies and safely retires in-flight writes
before acknowledging. Incomplete frames close their connection rather than
resuming with a different reply. CQEs/buffers remain owned until completion or
safe transport teardown. Reading application input never has to progress for
cancellation. Bytes already accepted by the kernel transport are delivered for
this contract: they cannot be recalled, even if the application reads them
later. Merely formatting a reply in a userspace buffer is not delivery.

The aggregate acknowledgement is usable once for the exact seat generation
and exact participant incarnations. Connection EOF alone is not proof a live
broker cancelled: it leaves quiescence unsatisfied. An orderly broker removal
cancels first; confirmed process death removes its future execution. Stale
acknowledgements, reconnects and replacement renderer instances cannot discharge
an older obligation. A new normal generation requires fresh registration/state
before clipboard operations resume. Committed clipboard contents survive;
uncompleted transfers do not.

Tapestry's independent coordinator continues the bounded cancellation/control
pump during QUIESCING; the main graphics loop may remain parked.
Lictor accepts that control during quiescence while keeping normal GPU/input
requests excluded. Neither loop waits synchronously for an application reply.

**Availability consequence:** a live but stalled renderer or Tapestry may prevent
SAK from opening. At the existing five-second deadline, the episode fails with
no grant, then follows the existing safe display restoration path. There is no
new forced process kill, bypass, infinite wait or silent downgrade. Keyboard
and scanout exclusion begins immediately; secret input starts only after the
aggregate acknowledgement. This adds a normal-session responsiveness dependency
to SAK and is the substantive policy choice approved on October 2.

The broker already holds its session's clipboard; this acknowledgement is a
cleanup obligation of that existing trusted implementation, not a new claim
that a malicious clipboard owner cannot retain data. Tapestry cannot fabricate
trusted key input or grant authority by acknowledging early.

Implementation follows a separate ratified scripture commit: bounded protocol
encoding/identity rules, owner-loop cancellation and participant lifecycle,
Lictor ACK gate, then native and graphical qualification. No kernel ABI addition
is proposed; if implementation requires one, reopen this review first.

## B. Preserve SAK independence; permit already-admitted normal work to finish (rejected)

Retain the current Lictor takeover path and explicitly weaken the clipboard
contract: work admitted by Tapestry before SAK may complete after takeover;
ordered notification cancels what remains when the broker observes it. Fresh
CHECKs are still denied outside the normal seat. This resembles the existing
focus-loss admission rule and avoids the renderer responsiveness dependency.

Cost: there is no immediate global copy/paste cancellation guarantee. An old
read can deliver ordinary clipboard text, or an old admitted commit can publish,
while the trusted scene is visible. Secret input remains excluded. This needs
an explicit specification change, not a claim that notification polling closes
the stronger requirement.

## Acceptance for A

Use deterministic schedules, not sleep lengths: CHECK/reply queued across
entry; get/read and commit in flight; partial transport writes; delayed, stale
and duplicate ACKs; registration racing entry; hidden sessions; participant
close/reconnect/death; one stalled participant; timeout/restoration; overflow;
fast successive episodes. Show cancellation and buffer retirement before the
last ACK, and trusted input impossible before that ACK. Prove no busy loop or
unbounded allocations during the cancellation pump.

Then exercise an enabled endpoint with two real clients and physical F10:
normal copy/paste, refusal during takeover, cancellation, recovery without stale
bytes, and no loss of the committed clipboard value. Capture the trusted scene
and restored workspace. Do not infer this coverage from existing media/SAK
screenshots. The October 1-2 heavy-gate waiver applies; ordinary CPU1 and focused
lifetime/isolation checks remain required.
