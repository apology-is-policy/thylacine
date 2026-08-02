---
id: inv-i37
type: inv
title: "I-37 — capability dataplane integrity: the grant is the mediation"
number: I-37
guards: [sub-kernel-weft]
validated-by: [spec-weft, spec-weft-readiness, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A per-flow shared page between a client and its server is sound, meaning all
five of:

1. **Registration is the capability.** Authority is established once, at grant.
2. **There is no per-operation mediation.** Nothing checks each transfer.
3. **The buffer lifetime is defended** — a page still in flight is never reused.
4. **The descriptor ring cannot reach outside the payload region.**
5. **The share is bounded by the flow** — it does not outlive it, and a claim
   after teardown fails closed.

Clause 2 is unusual: it asserts that a check does *not* happen. It is in the set
because the absence is the entire point — mediation is the cost the design
exists to remove — and because a reviewer's instinct is to add one back. The
model carries it as a named invariant with the instinctive fix as its
counterexample.

## Why it is stated this way

Isolation and speed are usually opposed. Here they are the same decision seen
from two sides: because the capability is established at grant, no per-operation
check is *needed*; because no per-operation check happens, the path is fast. The
invariant's job is to keep those two facts tied together, so that weakening
either — adding mediation, or loosening the grant — is visibly a change to both.

## Enforcement

**Admission is minted, never asserted.** A shareable region is either ordinary
anonymous memory or a device-passive framebuffer whose subtype bit is set only by
the allocation call that mints it. There is no flag a caller can set. Regions the
device *interprets* — command queues, descriptor tables — and memory-mapped I/O
stay structurally unshareable, so a shared page can never become a path to
hardware authority.

**The identifier never reaches the client.** The join key travels server-to-kernel
inside a round-trip the kernel itself initiated, so it cannot be forged, and a
claim consumes it exactly once. No region handle crosses Procs at all: the kernel
maps both sides, and the capability is holding the namespace-gated fid. Delegating
a handle instead would have created a duplicable cross-Proc reference to police.

**The pin is the lifetime.** Registration takes a reference the registry holds; a
claim transfers it to the binding; closing the fid drops it; the server's death
sweeps whatever was never claimed. The client's mapping is a separate reference
owned by its address space. The region frees when both reach zero, in either
order — and the refcount lock being per-region rather than per-Proc is what makes
the cross-Proc case identical to the two-thread case that was already proven.

**The geometry is private.** The kernel writes the shared header once for the
guest to read and never reads it back, keeping its own validated copy. A guest
scribbling its own header cannot move the kernel's idea of where the payload is.
Descriptor offsets are payload-relative, so clause 4 holds by construction rather
than by check.

**Clause 3 is the one with a gap.** The mechanism exists — a holder set covering
the server's stack, the device, and the peer acknowledgement, releasing the pin
exactly once on the emptying transition, with a stray or late clear a no-op — and
it is modelled and unit-tested. **Nothing arms it.** The server copies the ring
into its own buffer, so no page is ever in flight past its reply, and a single
terminal completion is the reusability signal. The clause therefore holds
*vacuously*: by avoidance, not by the defence built for it. See
[[seam-f-notif-unwired]].

**The client's pin is budgeted.** A dedicated per-Proc axis bounds how much a
client can hold mapped from elsewhere. The pages are the sharer's commitment, so
the client's ordinary page count is untouched; this separate axis is what bounds
the pin, including across a sharer crash.

## Validation

[[spec-weft]] carries the safety set model-first, with four counterexamples: the
premature release, the reviewer's per-operation re-check, the ring TOCTOU, and
the share outliving its flow. [[spec-weft-readiness]] carries the lock-free
readiness poke's no-lost-wake — [[inv-i9]] in shared-memory form. [[gate-smp]] is
the empirical backstop.

**blind-to:** clause 3's defence is unexercised, so the model verifies a mechanism
no production path reaches. Delivery is abstracted as initialization. The
framebuffer kind, its admission gate and the orphan reaper are prose plus tests.
And the userspace half of the readiness protocol is a hand-mirrored copy of the
kernel primitives, validated by construction rather than by being modelled —
which makes it the least mechanically checked part of a cross-Proc protocol.
