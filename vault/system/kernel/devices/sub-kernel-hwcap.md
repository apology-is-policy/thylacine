---
id: sub-kernel-hwcap
type: sub
parent: moc-kernel-devices
title: "Three ways to own a piece of hardware"
code:
  - kernel/mmio_handle.c
  - kernel/include/thylacine/mmio_handle.h
  - kernel/dma_handle.c
  - kernel/include/thylacine/dma_handle.h
  - kernel/pci_handle.c
  - kernel/include/thylacine/pci_handle.h
audit: hard
guarded-by: [inv-i5, inv-i32]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 13"
  - "docs/VIRTIO-PCI-DESIGN.md"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Lend a piece of hardware to a driver process, exclusively, and take it back when
the process dies. Three objects: a physical register range, a contiguous DMA
buffer, and a claimed bus function. Each is a handle whose whole content is *this
is mine and nobody else's*.

## Contract

Create one and you hold it until your last handle closes; nothing else in the
system can claim the same hardware while you do. The handle cannot be transferred
and cannot be duplicated ([[inv-i5]]) — so the creator is the owner, permanently
and singly.

Creation is gated twice: a capability that says this Proc may create hardware
handles at all, and — for a Proc whose authority has been narrowed — an allowance
naming exactly which ranges, interrupts, sizes and functions it may ask for
([[sub-kernel-allowance]]). Release is not a call anyone makes: it happens when
the handle table is torn down, which is what makes a driver's death sufficient to
free its hardware.

## Mechanism

**The interesting fact about these three is that they enforce the same exclusivity
three completely different ways**, because they differ in where the address comes
from.

**A register range is external to the allocator, so it needs a table.** A
fixed-size array of live claims, scanned under one lock for overlap before
anything is allocated. The overlap predicate is the symmetric one — two ranges
overlap iff each starts before the other ends — which admits *adjacency*: a range
beginning exactly where another ends is contiguous but distinct, and is allowed.
Creation also rejects a range that is unaligned, zero-length, wrapping, or past
the address width the translation tables can hold. That last check exists to
convert a fault into a refusal: without it the create would succeed and the
kernel would take an unhandled fault later, on the driver's first access, when the
mapping layer rejected the same address. **Rejecting at the door turns a kernel
death into a clean failure return** — the same move the area makes elsewhere.

**A DMA buffer comes from the allocator, so there is no table at all.** The page
allocator already hands out disjoint chunks; its partitioning *is* the exclusivity
mechanism, and the file says so where a claim table would otherwise be. This is
the batch's cleanest instance of an invariant enforced by an absence — there is no
code to inspect, and the property holds because of what allocation already means.
Buffers are zeroed on allocation, for two reasons that are stated separately: so a
driver bug cannot leak a previous user's bytes through descriptor padding, and so
a client's first view of a shared surface cannot be another surface's stale
pixels.

**A bus function is claimed by identity, and delegates.** The claim table here
holds bus/device/function triples rather than addresses; the register windows the
function decodes get their exclusivity by each becoming a *register-range claim*
in the first mechanism. So one object's exclusivity is built out of another's.

**Assignment, for a bus function, is done by us.** There is no firmware to have
done it, so the kernel sizes each window by the architectural dance — write all
ones with decoding off, read back which address bits are writable, invert and add
one — and hands out addresses from a bump arena seeded from the device tree. Two
details are load-bearing. The inversion is **width-correct**: a 32-bit window's
mask occupies only the low half, and inverting it at 64 bits would set the upper
half and yield a multi-exabyte size. And decoding stays off for the whole probe,
so the transient all-ones address a window briefly decodes is inert.

**Then the capability list is walked** to find the transport's regions, and every
region is validated against the window it names: the index must exist, the window
must have been assigned, and the region must fit inside the size that was actually
decoded. The walk is bounded by the address window capabilities live in and by a
hop counter, so a device offering a circular list terminates. This is the one
place in the area that reads a structure a device controls, and it is written as
if the device were hostile.

**Ordering, on the claim path, is what makes rollback total.** The exclusivity
slot is installed *before* any device state is touched — so a double claim and a
full table are refused while nothing has been mutated — and after that point every
failure path is a single unreference, which releases the windows and the slot
together. Enabling the device to decode and master the bus happens last, after
every window has a real address.

**Teardown quiesces before it releases.** Decoding and bus-mastering are disabled
*first*, then the window claims are dropped — because those addresses may be
handed to someone else, and a device still mastering the bus would write into
them. The window's pages themselves can outlive the claim: a live user mapping
holds an independent reference, so the address is freed for reuse only once that
mapping is gone too.

## Data structures

One object per lent thing, allocated zeroed, each with a magic value, an atomic
reference count, and its identifying fields set once at creation and never
rewritten. The register object holds an address and size; the buffer object holds
an address, size, page pointer, allocation order, and a create-immutable subtype
bit; the function object holds its triple, its per-window records, its resolved
regions, and its interrupt number.

Two claim tables, each a small fixed array under its own lock: thirty-two register
ranges, eight bus functions. The register table also holds the kernel's own
reservations, distinguished by a **sentinel owner value** rather than a separate
table — which works because the two operations that read the table want different
things: overlap-checking ignores ownership entirely, and owner-lookup is only ever
called with real object pointers, so it can never match the sentinel.

A bump arena for window addresses, seeded lazily from the device tree. It does not
reclaim: a freed window's address is never handed out again, and the argument is
headroom — the window is large enough for tens of thousands of claims against a
handful of live ones.

## Concurrency

Two locks, each guarding its own table, and **never nested**. The bus-function
lock is explicitly released before the register claim it needs is created, so the
two are taken in sequence rather than one inside the other. Both are taken
interrupt-safe because creation can run from kernel-context test code and release
runs from handle teardown in process context.

Reference counts are atomic, with release-acquire ordering on the decrement, and
only the caller that observes the one-to-zero edge frees. Every entry point
validates the magic value first, and every free clobbers it before returning the
memory, so a stale pointer dereferenced between the free and the allocator's
reuse of the slot stops on the magic check rather than reading plausible garbage.

The boot-time reservation path deliberately skips the lock, on the stated grounds
that it runs single-CPU before any other claim path is alive.

## Invariants enforced

**[[inv-i5]]** — this is the invariant's home. Non-transferability and
non-duplicability come from the kinds' membership in the hardware partition, which
is checked at compile time in [[sub-kernel-handle]]; exclusivity comes from the
three mechanisms above; the kernel's own hardware is protected by the pre-claimed
reservations.

**[[inv-i32]]** — partially, and with a gap that is deliberate. A buffer's pages
are **not** charged to the creating Proc's page budget; the bound is the
allowance's per-buffer ceiling instead. A cumulative per-driver budget is a
recorded future item, and until it exists the per-Proc page accounting does not
see DMA at all.

Mapping a window into a Proc belongs to the memory-object layer, and inherits its
rules — read-write only, never executable. That layer is not yet swept.

## Error paths

Refusing with a null return: not initialized; zero size; a misaligned or wrapping
range; a range past the translation width; an overlapping range; a full table; an
allocation failure; a size over the subtype's ceiling; an unknown or
already-claimed bus function; a malformed window or capability list.

Ending the world: initializing twice; referencing or releasing a corrupted object;
releasing below zero; freeing with references outstanding; freeing an object whose
claim slot has vanished; and filling the register table with kernel reservations
at boot, which is treated as a sizing error worth stopping for.

The asymmetry in the create path is deliberate and annotated: the two failures
before the slot is installed free the object directly instead of unreferencing it,
because there is no slot to release and no other holder — and the annotation warns
that any future state added before the slot is wired must be torn down there too,
since the unreference path expects a slot to exist.

## Performance

Linear scans of a thirty-two entry and an eight entry table under a lock; constant
time at this scale. Everything else is a handful of allocations. Claiming a bus
function is one-shot at driver startup.

## Prosecution

- The three exclusivity mechanisms are **different code for the same property**.
  A change to one proves nothing about the others, and the DMA one has no code to
  change — it holds because allocation partitions, which a future allocator
  sharing chunks would silently break.
- The reservation set must cover every range the running kernel touches. A new
  kernel-driven device that forgets to reserve is claimable by a capability
  holder.
- The sentinel owner works only while overlap-checking ignores ownership and
  owner-lookup is called with real pointers. A future scan that compares owners
  generically would match it.
- Ordering on the claim path: slot before device mutation, quiesce before
  release, enable last. Each is load-bearing and none is enforced by anything but
  reading.
- The two locks must stay unnested; the sequence, not the nesting, is what makes
  the order acyclic.
- The window-size inversion must stay width-correct.
- The capability walk must stay bounded and must keep validating regions against
  the decoded size — it is the only device-controlled structure in the area.
- The buffer allocator's rounding is computed by a **local copy** of the page-order
  helper, deliberately not shared. That is harmless while DMA is uncharged; it
  becomes a drift hazard the moment a cumulative budget lands, because the charge
  and the allocation would then be two independently-maintained answers to the
  same question — which is the failure the page-accounting elsewhere was fixed to
  avoid.

## Seams

The cumulative per-driver DMA budget does not exist; the per-buffer ceiling is the
only bound. The bump arena never reclaims. The virtio transport slots are
deliberately unreserved under a trust-boundary argument with a stated expiry —
see [[inv-i5]].

## Caveats

- **Every counter in these three files is read only by tests.** There are six
  accessors — created and live counts for each object — and across the whole
  kernel and architecture trees not one non-test caller exists. Three of them
  (two created-counts and the reservation count) have **no caller at all**. This
  is the area's organizing habit at its limit: where failure has no observer, the
  code counts things so that something can see them, and here the only thing that
  ever does is an assertion.
- Creating an interrupt handle does not require the right that waiting on it
  needs, so a caller can mint a handle that can never be waited on and only
  discover it at the first wait. Documented in the absorbed reference as
  deliberate — the model is treated as authoritative over the convenience.
- The register table is global rather than per-Proc; exclusivity is a system-wide
  property, so lookups scan every claim in the machine.
- One handle kind sits in the hardware partition with no implementation behind it,
  inheriting the partition's protections for an object that cannot yet be made.

## Provenance

Read from `kernel/mmio_handle.c` (471 lines), `kernel/dma_handle.c` (198),
`kernel/pci_handle.c` (469) and their headers, 2026-08-02, at `263650cd`.
Cross-checked: the handle-partition masks and their assertions, the capability
list, the struct size assertion, the allowance gates in the create handlers, the
counter call sites across the whole tree, and the thirty registered tests.

Absorbed `docs/reference/39-hw-handles.md` and `docs/reference/115-pci-claim.md`.
