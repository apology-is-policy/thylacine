---
id: sub-halcyond-application
type: sub
title: "Halcyon application request dispatcher"
parent: moc-userspace-shell-tui
code:
  - usr/halcyond/src/application.rs
  - tools/test-application-dispatch.py
audit: hard
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/HALCYON-INTERACTION.md", "docs/HALCYON-INTERACTION-CONTROLLERS.md", "docs/HALCYON-INTERACTION-ABI.md"]
created: 2026-10-03
updated: 2026-10-03
---
## Purpose

Dispatch typed HIN1 application operations through the existing Interaction owner.
This is the pure per-connection service component, not an activated native 9P
endpoint. [[sub-halcyond-interaction-record]] supplies transaction assembly and
replay; [[sub-halcyond]] owns controller and clipboard authority.

## Contract

Application::new takes a session and an injected kernel peer. open takes a
monotone local fid incarnation and the route resolved by the native adapter.
A connection pins one peer and one route for its lifetime, even when every fid
has been clunked. Eight fids share a controller and at most one pending HIA
admission. Peer identity is never accepted from an HIN1 body.

write assembles a record, validates its typed body, dispatches once or replays
an exact previous answer. The caller supplies the remaining input budget after
subtracting transport allocation; the dispatcher then subtracts all other fids.
decision requires freshly sampled peer metadata before delivering a matching
HIA receipt to Interaction. Dead/replaced peers retire before CHECK can publish.
complete accepts trusted local completion values from expiry/invalidation, not
wire receipts. Wrong targets cannot consume a pending transaction.

response returns a borrowed EncodedResponse. The native adapter copies requested
ranges directly into its accounted 9P output buffer. cancel retires a fid's
pending admission and replay bytes; clunk additionally removes that fid. retire
clears the connection's controller, pending operation and every record. The
caller separately retires unsent/partially sent transport frames before HSC ACK.

## Mechanism

Hello returns the session's fixed v1 limits. Bind uses trusted host observation
and a fresh peer through publish_on, returning Pending. Get, Begin and Commit
use the same Interaction CHECK sequence and pending target. Mode, Write, Read,
Cancel and Unbind resolve synchronously through that owner. Unbind forwards any
cancellation completion to the original pending fid, which can be a different
fid from the one carrying Unbind. All error answers remain replayable failures.

Read caches store scope, transfer, offset and count only. Each response reborrows
the admitted snapshot and revalidates scope and transfer expiry. A concurrent
copy cannot change that snapshot; cancelled/expired slots cannot leak stale
cached text. No Rc or Vec copy extends the snapshot past the store's two slots.

## Data structures

Application has a compile-time 4 KiB inline metadata ceiling, eight optional
Fids, one optional Pending, pinned Peer/Route and optional Scope. Fid retains a
Record<Result<Saved, Failure>> and the answered request ID. Saved is fixed
response metadata or read coordinates; it owns no heap buffer. Pending holds
an exact HIA Request, incarnation-bearing Record Ticket and broker Target.
Input accounting includes all record prefixes and body capacities. Output
cache allocation is zero; inline metadata is distinct from the transport buffer
ledger and from the 5 MiB session clipboard payload ceiling.

## Concurrency

The dedicated executor owns all mutations. No locks, new worker, syscall or
parallel HIA channel is introduced. One borrow of Interaction's snapshot spans
response encoding, preventing store mutation until that borrow ends. Cancellation
never frees a borrowed transport slot: exact completion or channel teardown
still drains it through Interaction's existing rules.

## Invariants enforced

- Complete validated requests precede effects; exact retries do not repeat them.
- Peer, route, fid incarnation and completion target cannot be substituted.
- Fresh peer loss precedes both publication and CHECK completion.
- Cross-fid Unbind resolves the pending cancellation rather than stranding it.
- Cached reads use admitted immutable snapshots and revalidate on access.
- SAK retirement releases record bytes and preserves request-ID high-water marks.
- Eight fids share the supplied connection input allowance.

## Error paths

Scope/session mismatch, stale peer, wrong completion, expired snapshot and stale
route registration refuse through existing Failure values. Pending admissions
retain their slot after cancellation until the exact receipt drains. Failed
Bind does not mint an active scope. A clunked provisional Publish cannot later
occupy a replacement fid's controller. A completed copy is not rolled back by
cancel or clunk. Native peer-sampling errors must close/retire the connection.

## Performance

No response payload allocation: EncodedResponse uses a 64-byte fixed prefix and
a borrowed slice. Copy work is bounded by the requested output range. Request
assembly/replay remains linear and bounded by the existing record limit. Host
tests establish bounds and behavior, not end-to-end latency or native RSS.

## Prosecution

Fifteen dispatcher tests exercise fragmented Bind, all ten operations, exact
Commit retry, changed retry poisoning, stale peers, fresh peer loss at CHECK,
clunk/reuse and late receipts, cross-fid Unbind, SAK record retirement, snapshot
expiry/cancel/owner loss, aggregate input and route pinning. Two injected clients
exercise immutable reads across another client's commit. Ten intended source
mutations fail their named assertions; compiler failures never count as witnesses.
The libhalcyon range encoder test uses an explicit wire fixture, every range
boundary, unchanged destination suffixes, overflow offset, and a borrowed-pointer
check. Broader Halcyon553 and libhalcyon169 host tests pass. Evidence is retained
in work/oct3-hi-dispatch with single-agent self-review.

## Seams

Native accepted-fd sampling, full 9P dispatch, shared completion routing,
servicepool admission, Tflush/tag mapping, transport-budget subtraction and HSC
partial-output retirement remain required. HI1-R24 is still open for the complete
native allocation ledger. Direct graphical surface ownership remains separate
from the existing sealed-terminal route contract.

## Caveats

Public interaction dispatch is still off. Injected two-client tests are not two
native processes, physical SAK cancellation, Pi qualification or a Main landing.
The renderer has no new visible modal workflow from this component alone.

## Provenance

Operator-approved HI-1 and strict pre-SAK cancellation contracts; transaction
foundation451ecb0d6. This checkpoint adds no ABI, kernel role or resource policy.
HI1-R26 cross-fid cancellation and HI1-R27 fresh CHECK peer retirement were fixed
and prosecuted before delivery. Original four authority/settings drafts remain
separate from the implementation and commit.
