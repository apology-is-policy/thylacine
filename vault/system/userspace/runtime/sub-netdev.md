---
id: sub-netdev
type: sub
title: "netdev — the NIC transport, and the one counterparty the kernel does not mediate"
parent: moc-userspace-runtime
code:
  - usr/lib/netdev/src/lib.rs
  - usr/lib/netdev/src/ring.rs
  - usr/lib/netdev/src/virtio.rs
  - usr/lib/netdev/src/virtio_pci.rs
  - usr/lib/netdev/Cargo.toml
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design:
  - docs/NET-DESIGN.md sections 13, 17
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The Ethernet frame transport underneath the network stack: a NIC driver that
hands its caller `send(frame)` and `poll_rx(buf)` and nothing else. Above it
[[sub-netd-nic]] wraps that pair as a smoltcp device and never learns which
kind of NIC it is talking to; below it lies a virtio device that writes
physical memory on its own schedule.

There are two transports — virtio over a flat MMIO register bank, and virtio
over PCI — presenting one API over one shared piece of index arithmetic. The
production network daemon drives the PCI one; the Menagerie driver-lifecycle
proof drives the MMIO one.

## Contract

- **`send(frame) -> bool`.** False for an empty frame, one over the maximum,
  or a ring still full after a reclaim attempt. A false is back-pressure, not
  an error: the caller retries after the device makes progress. The frame is
  copied out before return, so the caller's buffer is free immediately.
- **`poll_rx(out) -> Option<usize>`.** `None` when nothing is ready.
  Otherwise the frame byte count, clamped to `out.len()` — pass a
  maximum-frame buffer to never truncate. The descriptor is recycled before
  return, so `out` holds a private copy and the caller may keep it.
- **`drain_tx()`** reclaims completed transmissions; **`wait_irq() -> bool`**
  blocks until the device interrupts and reports whether it was ring progress
  rather than a config-change wake.
- **`quiesce()`** stops the device. Its contract is the one worth reading
  closely, and it is stated identically on both transports while being true
  of only one of them — see *Caveats*.

The two transports present the same method set deliberately, so a consumer is
written once. The PCI file states that it mirrors the MMIO file line for line
and that the only divergences are register access, queue notification and
interrupt acknowledgement.

## Mechanism

**Three layers, and the split is the point.** The index arithmetic lives
alone in a module that touches no memory and no device: it tracks the
producer/consumer counters a split-virtqueue driver maintains and returns the
ring *slot* to use. Both transports then add device glue — register layout,
the bring-up handshake, the descriptor and buffer memory. The arithmetic is
shared verbatim; nothing else is.

**Bring-up** follows the specified status ladder on both: reset, acknowledge,
driver, read the device's feature words, write back the intersection with
what this driver wants, latch features-OK and *read it back* (a device may
refuse), configure both queues, publish the receive buffers, then driver-OK.
A failure at any step writes the FAILED status bit before returning, so the
device is never left half-configured.

**Receive** is pre-posted and recycled: every buffer is published at
bring-up, drained one per call as the device fills them, and immediately
republished. Transmit is the mirror — descriptors are initialized once with
their buffer addresses and only the length is rewritten per frame.

**The two transports differ in what they are *given*, not only in how they
drive.** The MMIO driver is granted a physical window and an interrupt
number; it maps the window and reads registers at fixed offsets. The PCI
driver is granted a bus function and an interrupt number and *no memory
window at all* — its registers arrive through the BARs mapped off the claimed
function handle, and it locates each register group by walking the device's
capability list. A boot log shows the asymmetry plainly: the MMIO bind
reports one memory window and no bus function, the PCI bind reports one bus
function and zero memory windows.

**Notification** is where that difference surfaces at runtime. The MMIO
transport writes a queue index to one fixed doorbell register. The PCI
transport computes a *per-queue* doorbell address from an offset the device
itself supplies, scaled by a multiplier the device also supplies — so it
bounds that arithmetic against the notify region's reported length at
bring-up and refuses the device if a doorbell would land outside.
Acknowledgement differs the same way: MMIO reads a status register and writes
it back to a separate acknowledge register; PCI reads a single byte that
clears on read.

## Data structures

Two counter pairs and two device handles, and almost nothing else.

The transmit ring holds a size, a submission counter and a reclaim counter;
in-flight is their difference and back-pressure is that difference reaching
the size. The receive ring holds the same shape with its submission counter
*starting* at the ring size, because every buffer is published before the
device is armed.

Each transport's struct holds its RAII resource handles (the device handle,
the interrupt, and three separate memory regions — one for the ring
structures, one pool of transmit buffers, one of receive), the two ring
counter pairs, the cached hardware address, and the negotiated feature word.
The PCI one additionally caches the resolved addresses of each register group
and of the two per-queue doorbells, since those are not at fixed offsets.

The memory layout is fixed at compile time and *checked* at compile time: a
static block asserts that the six ring structures are non-overlapping, in
monotonic order, and fit their page, and that a buffer holds a full frame
plus the per-frame header. The PCI file adds two more assertions covering its
BAR window and its three memory regions.

The user-space addresses those regions map to are constants, and the two
transports deliberately use disjoint ranges so a consumer running both would
not collide. Two instances of the *same* transport in one process would.

## Concurrency

**There is no thread concurrency here at all**, and saying only that would
miss the whole story. Every method takes `&mut self`; the consumers are
single-threaded; there is no lock in the crate.

The concurrency that matters is with a **device**. It runs when we are not
running, it is not scheduled by us, and it does not stop when we stop. Every
barrier in the crate exists for it: a read barrier after each load of the
device's progress counter and before any read of the entry or buffer that
counter makes valid, and a full barrier before each doorbell write so the
descriptor and index updates are visible before the device is told to look.

That asymmetry is also the reason teardown is hard. A thread stops when its
process dies. An armed virtqueue does not — it is a standing instruction to
write specific physical addresses, and killing the driver does not retract
it.

## Invariants enforced

This crate holds no numbered invariant. What it enforces is narrower and
sharper: **every number the device supplies is bounded before it is used.**

- The descriptor identifier read out of the completion ring is checked
  against the ring size *before* it scales the buffer-pool base. It is the
  critical out-of-bounds guard, and it is checked in both transports.
- The device-reported length is clamped to the largest legal frame before any
  byte is read, so even a maximal lie is pinned inside the buffer it names;
  the copy is then bounded a second time by the caller's slice.
- Every ring slot the arithmetic returns is a counter modulo the size, so a
  slot can never leave the ring — a property stated in the module header and
  exercised across five full counter wraps by its tests.
- The transmit reclaim is clamped to the genuinely outstanding count, so a
  device reporting progress far ahead of what was submitted cannot drive the
  in-flight count below zero and wedge transmission forever.
- On the PCI transport, three region lengths the device reports are checked
  before any register in them is touched, and the per-queue doorbell offset
  is bounded within its region.

**And this is why the note is `audit: hard` in an area whose rule says
otherwise.** [[moc-userspace-runtime]]'s containment argument is that the
kernel validates everything handed to it, so a library bug corrupts its own
caller and nothing else. That argument names one party across the boundary.
Here there is a second: once the queues are armed the kernel does not mediate
a single byte the device writes. The premise of the area's argument is not
weakened here, it is **absent** — which makes this the area's third exception
for a third distinct reason, alongside the two libraries that compute an
authority.

The bounds above are therefore not defense in depth. For the descriptor
identifier and the reported length they are the *only* check that will ever
run — the same no-second-opinion shape as [[sub-warden]]'s grant arithmetic,
one layer further out, with a device in the place of a manifest.

## Error paths

**Bring-up** returns a specific reason for each way it can refuse — a claim
that failed, a device that is not a network card, a legacy register layout, a
missing version bit, refused features, a queue too small, an interrupt or
memory allocation that failed, and on PCI a missing register group or an
out-of-region doorbell. Every one of them either leaves the device explicitly
FAILED or never brought it up at all.

**A bogus descriptor identifier is dropped, not recycled.** When the
completion ring names an out-of-range descriptor, which buffer the device
actually filled is unknowable, so republishing a fabricated one could
double-post a live buffer. The entry is consumed and abandoned. The cost is
that each such lie permanently retires one receive buffer, so a device that
lies persistently starves its own receive path — safe, deliberate, and
recorded in the code as the closure of an earlier audit finding.

**Back-pressure is not an error path.** A full transmit ring self-drains once
and then returns false; the frame is dropped and the caller retries. The
stack above treats that as normal because it retransmits.

## Performance

The frame copy is **byte at a time in both directions** — roughly fifteen
hundred single-byte stores per transmit and as many loads per receive,
through the same single-instruction accessors used for device registers. That
is deliberate (one primitive serves both register and DMA memory, correct in
both) and it is the dominant per-frame cost in the crate.

The geometry is small: sixteen descriptors per queue, two-kilobyte buffers,
thirty-two kilobytes per pool, one page of ring structures. Sixteen frames
in flight is the transmit ceiling before back-pressure.

Note that the throughput figure recorded at [[sub-netd-nic]] does **not**
measure this crate — it is loopback, which never reaches a NIC. The
externally observed cost of this path is bounded by the consumer's poll
cadence, not by anything here.

## Prosecution

On any change, prosecute:

- **The four device-controlled inputs** — the descriptor identifier, the
  reported length, the queue size, and the PCI doorbell offset — each bounded
  before use, and bounded in *both* transports.
- **Barrier placement**: a read barrier after every load of the device's
  progress counter and before the reads it validates; a full barrier before
  every doorbell.
- **The recycle pairing**: exactly one republish per drained frame, or the
  receive queue runs dry.
- **The teardown fence**, which is the item most likely to be reasoned about
  wrongly — see the first two caveats before touching it.
- **Transport drift.** The hardenings are *duplicated* between the two files
  rather than shared, on a stated rationale (keeping the audited MMIO driver
  byte-identical, and presenting the PCI one as an independently auditable
  surface). They are in step today. But this is the same shape
  [[moc-userspace]] records for the three 9P servers, where a fix reached one
  sibling, half of another, and not the third — so a fix to one of these
  files is not a fix to the other, and nothing will say so.

The index arithmetic is the one layer in the driver framework with **real
host tests** — seven of them, two driving more than five full counter wraps,
one reproducing the lying-device case by name. They run under a feature flag
that drops the device glue, because the runtime does not build for the host.
That is the whole automated coverage; everything above the arithmetic is
proven only by the in-guest round trips.

## Seams

- **Interrupts are pin-based only.** Message-signalled interrupts are
  undriven: both the config and per-queue vectors are parked at the
  no-vector sentinel so the device routes through the legacy pin.
- **No offload of any kind** — the per-frame header is written as zeroes and
  skipped on receive; no segmentation offload, no checksum offload.
- **One instance per transport per process**, because the mapped addresses
  are constants.
- **Queue depth is fixed** at sixteen, negotiated only in the sense that a
  device offering fewer is refused.
- **The MMIO transport has no removal notification to hang a quiesce on** —
  [[sub-warden]] can revoke and terminate, but nothing reaches the driver
  first. This is what makes the third caveat below unresolvable rather than
  merely unresolved.

## Caveats

**The teardown fence is not where the documentation says it is.** Both files
tell a long-lived driver to call `quiesce()` itself, on the reasoning that
the supervisor's teardown is a forced group-terminate which skips the Rust
destructor, leaving a live device writing into pages the reap then frees. On
the **PCI** transport that obligation is already discharged by the kernel:
releasing the device handle clears the function's bus-master bit — disabling
its ability to initiate any transfer — before it releases anything else. The
production network daemon, the only PCI consumer, never calls `quiesce()`
and is nonetheless safe, for a reason neither document mentions. On the
**MMIO** transport there is no equivalent: releasing a memory-window handle
frees a claim slot and touches no device state, so the driver's own reset is
the entire fence. Two transports need two different answers; both files give
the same one, and it is the right answer only for the file where it is less
emphatically stated.

**The kernel's PCI fence protects the ring pages by side effect, in an order
nothing states.** Its stated purpose is to stop the function decoding memory
before its physical BAR ranges can be re-handed-out; stopping transfers into
the driver's own ring pages is a consequence, not the intent. It lands before
those pages are freed only because the handle table is released in ascending
slot order and bring-up claims the function *before* it allocates its three
memory regions. A driver that allocated its pools first — a natural choice,
failing fast on memory before touching hardware — or one that closed a lower
descriptor and then allocated, would free the pages first and leave a
bus-mastering device writing into them. The cheap anchor is one line at each
end: in bring-up, that the claim must precede the allocations; at the kernel
fence, what else depends on its position.

**The one caller that does honour the obligation honours it by quiescing
before it is useful.** The Menagerie driver resets the device, *then* signals
readiness, then blocks forever on an interrupt a reset device will never
raise. That is correct for what it is — a lifecycle proof whose expected end
is exactly the forced teardown — but it is not a pattern a driver carrying
traffic can copy, since such a driver must keep the device live and its only
correct moment to quiesce is on notice of removal. No such notice exists.

**The transmit reclaim clamp bounds the counters, not the buffers.** A device
reporting progress *behind* the driver's cursor over-reclaims up to the
in-flight count, freeing transmit slots the device may still be reading. But
that is the same exposure a device gets by simply claiming everything is
complete, which the clamp's own test asserts as the desired outcome —
"send is not wedged". The guarantee is against wedging, not against buffer
reuse; a lying device can always make the driver overwrite a buffer it is
still reading, and the damage is confined to the driver's own outbound bytes.

**The MMIO module header describes a constructor that does not exist** — one
that probes each page of the register bank, keeps the one reporting a network
device, and releases the rest. The file has only the grant-driven constructor,
which is told its slot and probes nothing; the retirement is recorded forty
lines above, in the same file. Worse, the paragraph's still-live half — that
network and block devices share a register page — names the wrong pair of
programs. Task #143.

**The MMIO grant is a page, and the page is shared.** A register window is
mapped page-granular, so the allowance is page-rounded and this driver claims
the whole page containing its slot — which on the reference machine also
contains two block-device slots. Memory-window claims are page-exclusive, and
the filesystem server probes that bank page by page for its disk. It works
today only because the Menagerie driver is torn down before the pivot; making
it persistent, which is the recorded next step for it, would lock the
filesystem server out of its disk. Task #142.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
