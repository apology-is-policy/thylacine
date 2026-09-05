---
id: inv-i40
type: inv
title: "I-40 — a shared pixel page is never freed under a holder, and never claimed after it dies"
number: I-40
guards: [sub-tapestryd, sub-kernel-weft]
validated-by: [spec-tapestry-present, prose, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A compositor surface's pixels live in a page shared between three
independent holders — the compositor's own mapping, the client's
mapping, and the display hardware reading it by DMA. I-40 is that no
holder ever loses the page out from under itself:

- **Recycle at completion, not at submit.** A client may reuse a buffer
  slot only once the present naming it has *terminally* completed. An
  acknowledgement that the request was received is not that.
- **Retire in order.** Tearing down a surface disarms its share
  registration *before* freeing anything, releases scanout before the
  resource, and kills the resource before its backing.
- **No stale claim.** A claim token that names a retiring or retired
  weave resolves to nothing and fails closed. The claim is
  consume-once.
- **Reweave keeps the old generation alive until the new one is
  displayed.** The displaced generation is never read again but stays
  shown, and dies only at the first present that proves the new one has
  taken over.

The share itself — a page mapped into two address spaces at once — is
[[inv-i37]]'s and the cross-process refcount's. I-40 is what the
*compositor* must do so that machinery is never asked the impossible.

## Enforcement

**Two halves.** The kernel mints the share-admissible allocation subtype
and owns the consume-once claim and the reaper; [[sub-tapestryd]] owns
the lifecycle above it.

**The retire ordering is the invariant made executable** — five steps in
[[sub-tapestryd]], and the reason the unshare comes second is precisely
the no-stale-claim clause: a claim racing the teardown must find the
registration already gone. The pages themselves outlive the server's
release, because the client's mapping still holds a reference; they free
when both drop, or when the kernel reaper force-reclaims an orphaned
mapping after the compositor itself dies.

**The quiesce is discharged by construction, and that is the fragile
part.** Every present's transfer window opens and closes inside a single
protocol dispatch, because the command engine is synchronous — so the
in-flight set is provably empty at every retire decision point, without a
drain. The guard is real; the code implementing it does not exist,
because nothing can be in flight. **A pipelined command queue does not
falsify the guard — it un-implements it**, silently, with the model
still green. That is the one change to this surface that must land a
real drain first.

**The reweave fence is reply ordering**, not tagging: the acknowledgement
completes only after the new generation is allocated, and the connection's
frame stream is FIFO, so a client that has read the ack cannot still have
a stale-geometry present arriving. At most one generation drains; a second
reweave is refused.

**The untrusted edge** is the present descriptor itself: version, rect
count, exact payload length, slot index, and every rect validated in
wide arithmetic *before* any pixel work. A partially-applied malformed
present is worse than a rejected one.

## Validation

[[spec-tapestry-present]] — `RecycleGate`, `NoTornScanout`,
`DisplayedBacked`, `NoStaleMap` and `ReweaveOrdered`, with four buggy
cfgs, one per forgotten holder. The per-boot pattern gate drives the
full path with a liveness double-dump.

**blind-to:** everything about pixels — the composed blit, damage
rects, letterboxing — and the pane tree, the event queue policy, and the
per-session ownership gate, which is an isolation property rather than a
lifetime one. The virtio command engine's own correctness is prose plus
the focused audit; its completion authority is the used ring and never
the interrupt status bit, a distinction that once cost a permanent
engine desync.
