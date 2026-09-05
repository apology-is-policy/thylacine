---
id: sub-kernel-weft
type: sub
parent: moc-kernel-async
title: "Weft — one page, two Procs, no copy"
code:
  - kernel/weft.c
  - kernel/include/thylacine/weft.h
audit: hard
guarded-by: [inv-i37, inv-i30, inv-i9, inv-i32]
validated-by: [spec-weft, spec-weft-readiness, gate-smp]
locks: []
abis: []
design:
  - "docs/NET-THROUGHPUT.md"
  - "docs/reference/125-weft.md"
created: 2026-08-02
updated: 2026-08-24
---
## Purpose

A confined Proc's network bytes normally cross into the network daemon by being
copied — into a kernel buffer, out again. Weft replaces the copy with a page both
Procs map: the daemon does the capability work once, at grant, and the bytes then
move through shared memory with nothing mediating each operation.

The isolation is the grant. The speed is the *absence* of per-operation
mediation. Those are the same design decision seen from two sides, and the rest
of the mechanism exists to make the second one safe.

This is also the tree's first shared page. Before it, no region was reachable
from two Procs at once.

## Contract

Three calls, and none of them hands a shared object to anyone:

- **Share** — a driver-tier Proc registers a region it already owns and receives
  a kernel-scoped identifier. Gated on the hardware-creation capability, because
  the registry is a small fixed global and an ungated caller could squat it.
- **Map** — a Proc holding a flow's data fid asks for the flow's region. The
  kernel asks the *server* for the identifier over its own protocol round-trip,
  claims it, and maps the region in. Idempotent: a second call returns the same
  address.
- **Unshare** — the registrant disarms one of its own unclaimed identifiers.

The identifier never reaches the client. It travels server-to-kernel inside a
round-trip the kernel initiated, so a client cannot forge one, and a claim
consumes it exactly once. That is the remote-memory-key shape: possession of the
key is the authority, so the key never leaves the trusted path.

Nothing transfers a region *handle* between Procs. The kernel maps both sides;
the capability is holding the namespace-gated fid. That was the deliberate choice
over delegating a handle, which would have created a duplicable cross-Proc
reference to police.

## Mechanism

### Admission is minted, never asserted

A shareable region is one of exactly four kinds, and the kernel decides which by
reading the region's *type*, not a flag its creator set:

- an ordinary anonymous region — a flow ring;
- a device-passive framebuffer region, which the device **reads**;
- a graphics buffer object, which the device **writes**;
- a host-visible hostmem region — the Warp Model-B GPU command ring, which the
  host GPU process **reads**.

The middle two are distinguished by immutable subtype bits (`weave` / `gpu_bo`)
set only by the allocation calls that mint them on a `kobj_dma`. The fourth is not
a `kobj_dma` subtype at all: it is keyed on the burrow *type* being
`BURROW_TYPE_HOSTMEM` carrying a `kobj_pci` — a PCI hostmem-BAR window whose bytes
a guest maps and the host GPU process reads as a Venus command ring (see
[[sub-tapestryd]]). Everything else — a plain device region holding a command
queue or descriptor table, and all memory-mapped I/O — is structurally
unshareable. There is no flag a caller can set to admit one. Sharing a region the
device *interprets* is therefore impossible by construction, which is the point:
the discriminant is minted at allocation by the kernel (a subtype bit, or the
burrow type) and is create-immutable.

**The hostmem kind must be widened in lockstep at BOTH reading sites, and once it
was not.** Admission is read twice — `weft_claimed_kind` (the claim-time
cross-check) and `weft_binding_alloc_maponly` (the binding allocation) — and a kind
the first admits but the second rejects is not half-shareable, it is a fid that
claims clean then maps to nothing. V-2 added the `HOSTMEM` arm to
`weft_claimed_kind` and missed `weft_binding_alloc_maponly`, so a claimed hostmem
share fell through to a NULL binding (`t_weft_map` returned -1) — a latent
half-widen closed by Warp V-3b-1c-2b's F1, which added the matching binding-alloc
arm. The two sites widen together or the kind is broken; that lockstep is the
invariant, and the half-widen was the bug.

**The direction of device access is what separates the two device kinds, and
neither direction weakens the admission argument.** What matters is that the
device treats the region as *data* — pixels in, pixels out — rather than as
instructions to execute.

The claim path re-derives the kind from the type and cross-checks it against the
server's declared geometry — a ring must declare descriptor slots, and **every**
map-only kind must declare none (framebuffer, graphics buffer, and the hostmem
ring alike), since none has a descriptor ring and a declared geometry over one is
a contradiction. A server whose declaration contradicts its own registered region
fails closed and is never mapped.

**The two *DMA* device kinds are read by test order, in two places here**, and the
ordering is unambiguous only because the allocator cannot mint a region that is
both. That guarantee lives in [[sub-kernel-hwcap]]'s constructor — an enumerated
argument, so the illegal combination cannot be written at the call — and nothing
at either reading site says so. Verified: that constructor is the sole writer of
either bit across the kernel. It holds, and it holds at a distance. The hostmem
kind sidesteps this ambiguity entirely: it is keyed on `type ==
BURROW_TYPE_HOSTMEM` (with a `kobj_pci`), a discriminant disjoint from the
`kobj_dma` subtype bits, so its arm cannot collide with the weave/gpu_bo test
order — it is a separate branch, admitted or refused on its own terms.

### The pin is the lifetime

Registration takes a reference on the region and the registry holds it. A claim
*transfers* that reference to the binding recorded against the data fid; closing
the fid drops it. So a region that a server registered but the kernel never
claimed cannot leak: either a claim takes ownership, or the server's death
sweeps it.

The client's *mapping* is a separate reference, owned by its address space and
reclaimed when it exits. The region frees when both reach zero, in either order.
The refcount lock is per-region rather than per-Proc, so it serializes the two
Procs' teardown races exactly as it already serialized two threads' — which is
the whole cross-Proc proof, and the reason the substrate needed almost no new
code.

### A private view of the geometry

The shared page begins with a header carrying magic, slot count and region
offsets. The kernel writes it once, at grant, for the guest to read — and then
never reads it back. It keeps a private view holding the same geometry, computed
from the region's contiguous kernel address and validated at layout time, and
every bounds check on the hot path reads *that*.

A guest scribbling its own header therefore cannot move the kernel's idea of
where the payload region is. It is the same discipline as [[sub-kernel-loom]]'s
private counters, applied to a geometry instead of an index.

### Descriptors are copied before they are believed

The guest produces `{offset, length}` descriptors into a split ring. The drain
copies each into kernel memory, validates it — reserved flags clear, non-zero
length, and the extent inside the payload region with the sum computed wide so a
hostile pair cannot wrap back in-bounds — and emits only validated snapshots.
A rejection increments a counter rather than failing the drain.

The producer's tail is acquire-loaded, pairing with the guest's release-store
after it finishes writing a slot, so a half-written descriptor is never read.
The guest mutating a slot *after* posting it is a different problem, and the
snapshot is what defends that one.

### The readiness poke

Waiting for the network by polling costs a syscall and latency. Instead the
producer bumps a counter in a shared cache line and the consumer reads it at
memory speed. This is the push counterpart to the elicited-readiness pull the
9P poll bridge does.

The no-lost-wake argument is the store-buffer litmus test, in shared memory
across a Proc boundary. Before parking, the consumer publishes its intent and
re-reads the counter; on an edge, the producer bumps the counter and reads the
intent. Each side does a sequentially-consistent store then load on *opposite*
words, so in the global order at least one sees the other's write — and an edge
arriving in the parking window is never lost.

The two word-pairs sit on separate cache lines and each has a single writer.
The producer never writes the consumer's words even to wake it; the wake is a
separate rendezvous.

### The completion contract for a true zero-copy send

A zero-copy send completing means only "queued": the page may still be read by
the network device, and for a reliable stream it stays live until the peer
acknowledges. Releasing the pin when the operation completes reuses a page that
is still in flight — the exact use-after-free that io_uring's buffer-notification
mechanism exists to prevent.

So the design holds the pin until the *last* of three holders clears — the
daemon's stack, the device, the peer acknowledgement — and only then posts a
second, notification completion. The tracker's holder set is the complete state:
in flight if and only if it is non-empty, release emitted exactly once on the
emptying transition, and a stray or duplicate or late clear is a no-op. A caller
that releases only on that signal cannot release early, by construction.

This mechanism is built, modelled and tested. It has **no production caller** —
see [[seam-f-notif-unwired]].

### The orphan reaper

If the compositor serving a framebuffer dies, a client's mapping keeps the pixel
pages alive but semantically dead. Without intervention the client pins them
until it exits.

A kernel thread sweeps registered framebuffer bindings; one whose serving session
has been dead past a grace period is force-reclaimed — the client's stale mapping
is unmapped cross-Proc, its budget uncharged, the pin dropped. The client was
warned; a later touch faults.

The reaper parks indefinitely on an empty registry rather than ticking, so an
idle machine has no periodic wakeup.

Its liveness test reads the session's dead latch, which only an *active* receive
path sets. So the guarantee is narrower than it looks: it reclaims a client whose
session **observed** the death. One that maps and then never touches the session
again is never observed dead, and rides the budget and exit-time teardown
instead — bounded, just not early.

## Data structures

**The shared region** — a header, the readiness cache-line pair, the descriptor
array, then the payload. Descriptor offsets are payload-relative, so a descriptor
*cannot* address the header or the descriptor array however it is crafted.

**The private view** — the trusted base, size, slot count, offsets, and the
consumer position, all kernel-only.

**The share registry** — a 64-entry table of `{identifier, region, owner}`,
identifiers monotonic and never zero.

**A binding** — the region and its pin, the guest address as a weak record, the
size, the kind, the mapping Proc's pid, the private view (zero for a
framebuffer), and the reaper's linkage.

**The holder tracker** — two words, kernel-private, never on the shared page. The
client sees only the completions.

Every ABI structure's size and load-bearing offsets are pinned by compile-time
assertion.

## Concurrency

**The registry lock is a pure leaf.** Every reference take and drop happens
outside it, because dropping one can free a region and reach the page allocator.
Registration takes its reference before publishing the entry, so a slot never
names a region it does not hold alive.

**Cross-Proc mapping** composes the existing map path, which was already
Proc-agnostic and already serialized the dual refcount under the per-region lock.
Each Proc takes its own address-space lock then the region lock; those are
distinct per-Proc locks converging on one inner lock, so there is no cycle.

**The map install is a compare-and-swap** against the fid's binding slot. Two
threads of one guest mapping the same fid each build a binding from a distinct
identifier; exactly one wins, and the loser tears its own down and returns the
winner's address.

**The reaper's order** is registry lock, then the process table, then the target's
address-space lock — acquired under the table lock and *held past its release*,
so the per-page unmap runs with interrupts on rather than inside the table's
interrupt-off window. Registration and unregistration both run lock-free. The
close path unregisters before reading the binding, and the reaper nulls the
region pointer under the registry lock, so neither side sees a half-reclaimed
binding.

**The unmap-at-close guard** re-checks that the address still holds *this*
region. The recorded address is a record, not a claim: after an explicit detach
an unrelated mapping can land there, and it must survive the close untouched.

## Invariants enforced

**[[inv-i37]]** — dataplane integrity. Registration is the capability, there is
no per-operation mediation, the buffer lifetime is defended, the descriptor ring
cannot be used to reach outside the payload, and the share is bounded by the
flow.

**[[inv-i30]]** — the snapshot discipline, lifted from the operation descriptor
to the payload descriptor: copy, validate, act on the copy.

**[[inv-i9]]** — the readiness poke's no-lost-wake, in the shared-memory form.

**I-5**, whose own home is the handle table and is not yet swept: regions the
device *interprets* stay unshareable, so a shared page never becomes a path to
hardware authority. The kernel-minted admission gate is what carries that here —
recorded as a claim this area upholds rather than one it owns.

**[[inv-i32]]** — the client's cross-Proc pin is charged to a dedicated per-Proc
budget. The pages are the *sharer's* commitment, so the client's ordinary page
count is untouched; this separate axis is what bounds a client's pin, including
across a sharer crash.

**And the sharer's own charge had nothing settling it at all.** The daemon
detaches its ring when a flow closes — every closed zero-copy flow — while the
guest's mapping and the binding's pin live on. So the drop that finally frees the
pages is the *guest's* address-space teardown: generic code, in another process,
holding that process's lock, with no way to name who paid. Sixty-four pages per
closed flow, monotonically.

The release rule is now: the sharer settles **when the region is shared out and
this process has unmapped it**, whether or not the pages freed. Once it has
handed the region across and let go of its own view, it cannot reach those pages,
and charging a process for memory it cannot touch caps it for nothing — from
there the consumer's own shared-mapping axis accounts them.

**The discriminator is deliberately "shared out" and not "does anything still
hold this."** A process's *own* other claim — a ring registered into its own
async ring, say — also keeps the region alive, and there the charge must stay
until that claim drops. The broader test would release it early, which is the
budget-inflating direction.

Worth recording plainly: **the leak breached no bound only because the daemon
happens to be exempt.** That exemption follows from an identity chain granted for
entirely unrelated reasons, so the safety was a coincidence of two independent
gates rather than a property anyone enforced — and the first non-exempt driver
would have made it a live monotonic leak.

## Error paths

`-1` from share for a non-driver caller, a zero or oversize length, an
unresolvable or non-whole-region address, an inadmissible region type, an
executable mapping, or a full registry — in which case the flow simply stays on
the copy path. `-1` from map for a non-9P fid, a server without the round-trip,
a bad or replayed or already-disarmed identifier, a kind mismatch, no address
space, or allocation failure. `-1` from unshare for an identifier that is not a
live entry of the caller's.

Validation returns `-1` for a framebuffer binding, an address below the payload
region, an offset outside the descriptor domain, or a failing bounds check — and
every caller reads that as "fall back to copying", not as an error.

## Performance

The measured result is worth stating carefully, because an earlier claim in the
scripture was wrong and was corrected. Against the copy path at matched size the
aggregate is a **dead heat**, not a win and not the "10× slower" once recorded.
The data movement itself is about twice as fast — the same per-operation cost
over roughly half as many operations, since a push absorbs more than the send
window.

Both paths spend about 95% of their time in the same place: a bulk sender fills
the window and waits for the writable edge. That stall is transport-independent
and is a registered seam elsewhere; it is not a cost of this mechanism.

The threshold below which payloads stay on the copy path is 1 KiB — the same
hybrid split the orthodox 9P transport uses.

## Prosecution

- **Admission stays kernel-minted.** Anonymous, or the allocation-time
  framebuffer subtype. Never a creator-asserted flag, and never "any device
  region".
- **The kind is derived from the type**, and the server's declared geometry must
  agree or the claim fails closed.
- **The kind gate on validation is the single chokepoint** closing all three
  data-drive consumers for a framebuffer binding. Splitting it per-consumer
  reopens the gap it closes.
- **The pin balances exactly once** — transferred at claim, dropped at close, or
  swept at owner death. Reference operations stay outside the registry lock.
- **The geometry is read from the private view, never the shared header.**
- **A descriptor is copied and validated before use**, with the extent sum
  computed wide.
- **The budget charge and uncharge pair on the mapping flag**, so every teardown
  of a shared mapping uncharges.
- **The sharer settles on "shared out", never on "still held".** Broadening that
  test to any surviving reference releases the charge while the process's own
  claim is still live — an under-count, which is the direction that breaks the
  bound.
- **A refund goes to the recorded payer, not to whoever is dropping.** Claim
  against the region before the drop, restore if it did not free; the reaper's
  sweep runs the same protocol, because the owner it is sweeping is not
  necessarily the payer.
- **The two device kinds are disambiguated by test order** in both the kind
  derivation and the binding allocation. That is sound only while the two subtype
  bits are mutually exclusive by construction, which is enforced in another
  subsystem's constructor and stated at neither reader.
- **The close-time unmap keeps its identity guard.** Trusting the recorded
  address alone tears down whatever now lives there.
- **The reaper unregisters before the close reads the binding**, under the same
  lock. Reordering those is a use-after-free.
- **The readiness words keep one writer each**, and both sides keep the
  sequentially-consistent store-then-load. Weakening either ordering reopens the
  lost wake.
- **The pin releases at the notification, never at the operation**, if and when
  the holder tracker is wired.

## Seams

- **[[seam-f-notif-unwired]]** — the buffer-lifetime tracker has no production
  caller; today's safety comes from the daemon copying.
- **The reaper only sees observed deaths.** A client that maps and goes quiet is
  reclaimed at its own exit rather than at the grace.
- **A transient allocation failure during map pins a flow to the copy path
  permanently**, because the identifier was consumed but no binding installed and
  the idempotent return re-reports it.
- **The mapping-Proc identity is a bare pid.** Sound while pids are monotonic and
  unreused, which is the tree-wide precedent.

## Caveats

- **Both files describe themselves as the third sub-chunk.** The header says
  "this header is the substrate: the descriptor-ring ABI and the validating
  consumer" and lists the delivery calls as future work; the file's opening line
  says the same and states that the delivery "is Weft-6". The share registry, the
  claim path, the framebuffer kind and the orphan reaper are all *in these two
  files*. Third consecutive area where the summarizing prose describes an earlier
  version while the comments beside the code are current and carry their audit
  references.

## Provenance

[[chg-2026-08-02-async-sweep]], [[chg-2026-08-16-weft-third-kind]].
