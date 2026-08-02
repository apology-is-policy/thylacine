---
id: inv-i5
type: inv
title: "I-5 — a handle naming hardware never leaves the Proc that made it"
number: I-5
guards: [sub-kernel-hwcap, sub-kernel-handle, sub-kernel-gic, sub-kernel-discovery]
validated-by: [prose, gate-smp]
strength: spec
created: 2026-08-02
updated: 2026-08-02
---
## Statement

A handle that names a piece of hardware — a physical address range, an interrupt
number, a DMA buffer, a claimed bus function — **cannot be transferred to another
Proc and cannot be duplicated**. The Proc that created it is the only Proc that
will ever hold it, for as long as it holds it.

## Why it is stated this way

The obvious reading is a confinement rule: don't let driver authority spread. The
sharper reading is that **these handles are claims, and a claim you can copy is
not a claim.**

Every one of these objects exists to say *this range is mine and nobody else's*.
That exclusivity is not a property the handle carries; it is the entire content of
what the handle means. So duplicating one would produce two holders of a token
whose meaning is "there is exactly one holder" — the object would be internally
false the moment it was copied. Transfer has the same shape one step out: the
claim was granted against the creating Proc's authority, and moving it would
leave the claim asserted on behalf of a Proc that no longer holds it.

The concrete failure is ordinary and severe: two Procs mapping one register bank
and writing it without coordination. Nothing in the hardware objects to that. The
first symptom would be a device wedged in a state neither driver believes it is
in — which, in this area, is a failure with no observer ([[moc-kernel-devices]]).

## Enforcement

**By partition membership, not by a check at the transfer site.** Every handle
kind is classified into exactly one of four disjoint sets — transferable,
hardware, service, ring — and the transfer and duplicate paths ask which set a
kind is in rather than naming kinds individually. A hardware kind therefore gets
non-transferability *and* non-duplicability by being listed, with no per-kind code
anywhere. When the bus-function handle was added it joined the hardware set and
inherited both properties in the same line that declared it.

**The classification is total, and the compiler checks that.** Assertions pin the
sets pairwise disjoint, and one more asserts their union is every kind except the
invalid sentinel. So a new kind that is added and classified nowhere does not
quietly default to transferable — **it fails the build**. The property most likely
to erode here is "someone adds a kind and forgets", and that is precisely the case
the completeness assertion refuses. The formal model states the partition; the
assertions are what keep the code's classification faithful to it.

**Exclusivity itself is enforced three different ways**, because the three kinds
of hardware differ in where the address comes from. A register range is external
to the kernel's allocator, so it needs an explicit table scanned for overlap. A
DMA buffer comes *from* the allocator, so the allocator's own partitioning is
already the claim and no table exists. A bus function is claimed by identity, and
its register windows delegate their exclusivity to the first mechanism. Same
invariant, three enforcements, and the middle one is enforced by an absence.
[[sub-kernel-hwcap]] holds the detail.

**The kernel's own hardware is claimed before userspace can ask for it.** The
ranges the running kernel uses — interrupt controller, console, clock, config
space — are pre-claimed at boot with a sentinel owner, so a driver's request for
an overlapping range is refused by the same overlap check that separates two
drivers. Without this, a Proc holding the hardware-creation capability could claim
the interrupt controller's own registers and write live acknowledgement state.

**One relaxation is deliberate and argued.** The virtio transport slots are *not*
pre-claimed, because the kernel only reads them during boot enumeration and they
exist to be driven by driver processes. Reserving them would have required a
delegation API to hand each one back.

That argument understates the difficulty: the exemption is also **structural**.
Reservation and claiming both work at page granularity, and the transport slots
are packed eight to a page ([[sub-kernel-discovery]]) — so a driver claiming its
own slot *necessarily* claims seven neighbours, and no reservation could take
one without taking all eight. The live configuration depends on this: the
kernel's entropy source shares a page with a userspace driver's device, which is
why the death-time quiesce has to exclude that device **by identity rather than
by ownership**. A better delegation API alone would not have avoided this.

The argument depends on a trust boundary —
that only the root of trust grants the hardware capability — and it is recorded
with its own expiry condition: if that grant ever becomes more permissive, the
relaxation must be revisited. One slot has since drifted from the argument's
premise (the kernel now drives the entropy source rather than merely probing it)
and is documented as sharing the same residual rather than being quietly folded
in.

## Validation

The disjointness and completeness assertions are compile-time. The formal model
is `specs/handles.tla`, whose hardware partition and its exclusivity property this
implements. At runtime the evidence is a claim-then-reclaim test: a driver Proc
takes a range and an interrupt, exits, and a second Proc claims the same hardware
successfully — which proves the release path cleared both claim tables, and is the
only end-to-end check that the invariant survives a Proc's death.

**blind-to:** the compile-time half proves a kind cannot be *classified* wrong; it
proves nothing about a future transfer path that forgets to consult the
classification at all. There is one transfer surface today and one duplicate
surface, both of which ask. A third — a new syscall that moves a handle by some
other route — would not be caught by any of these assertions. The invariant's
enforcement is total over the kinds and only as good as the census over the paths.
