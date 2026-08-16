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
guarded-by: [inv-i5, inv-i32, inv-i34]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/ARCHITECTURE.md section 13"
  - "docs/VIRTIO-PCI-DESIGN.md"
created: 2026-08-02
updated: 2026-08-16
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

### The subtype the caller cannot get wrong

A DMA buffer carries a kernel-minted class — plain, a compositor's framebuffer
(device-*read*), or a GPU buffer object (device-*written*) — fixed at creation
and never rewritten. Each class carries its own size ceiling.

The classes are stored as **separate flag bits** but selected through a single
**enumerated** constructor argument, and the reason is stated where it is
enforced: with one boolean per class, a caller could pass both, and the struct
would hold a state with no meaning. With an enum it cannot be expressed at the
call. The bits remain independent in memory; only the door is narrow.

This is the third distinct place in the tree where **the shape of an interface
is the safety property** rather than a check inside it — alongside the charge
claim that returns pages instead of a record, and the share-drop that returns a
verdict instead of a count. In each, the wrong call is not rejected at runtime;
it cannot be written.

The cost is a coupling that neither end declares. A consumer in the network
dataplane maps a buffer's class to a binding kind with an ordered
if-else — framebuffer tested first, GPU buffer second, anything else refused —
which is unambiguous **only because both bits can never be set at once**. That
guarantee lives in this file, in a constructor; the reader is in another
subsystem, and its comment carefully justifies re-checking that the region is
admissible while saying nothing about why the ordering is safe. Verified: the
constructor is the sole writer of either bit in the whole kernel, so the
guarantee holds today. It is an argument, held at a distance, with no tripwire
between the two ends.

**Assignment, for a bus function, is done by us.** There is no firmware to have
done it, so the kernel sizes each window by the architectural dance — write all
ones with decoding off, read back which address bits are writable, invert and add
one — and hands out addresses from bump arenas seeded from the device tree. Two
details are load-bearing. The inversion is **width-correct**: a 32-bit window's
mask occupies only the low half, and inverting it at 64 bits would set the upper
half and yield a multi-exabyte size. And decoding stays off for the whole probe,
so the transient all-ones address a window briefly decodes is inert.

**Placement is a second width question, and it is not the same one.** There are
two arenas — the host bridge's low window, and a high one — because a GPU asked
to expose host memory presents a window of several gigabytes, which the low
window (well under a gigabyte on the reference machine) structurally cannot
hold. Routing picks by **size**, so a small window that merely *could* live high
still lands low and every existing driver keeps the addresses it had.

Size alone is not sufficient, though, and this is where it went wrong. A window
that is too large for the low arena but is *not* 64-bit-capable must fail rather
than move: only its low half is ever written back, so a high address would be
truncated on the way to the device — which would then decode a truncated address
somewhere inside RAM while the kernel's exclusivity claim sat on the untruncated
one. Two views of the same window, disagreeing, with the device's view pointing
at memory. So placement takes the capability bit as well as the size, and a
32-bit window that will not fit low fails honestly.

**The two width rules are about different things and neither implies the
other.** One governs *decoding a size*, the other *writing an address*. They live
in the same file, they are the same 32-versus-64 confusion, and having got the
first right is no evidence at all about the second.

A third bug sat in the same routing: the low arena's fit test compared the
request against the window's **total span** rather than what remained, so once
the arena was exhausted every later window failed there instead of falling
through to the high one. Capacity is not availability, and a full container that
reports its size still answers "yes, that fits."

**Then the capability list is walked** to find the transport's regions, and every
region is validated against the window it names: the index must exist, the window
must have been assigned, and the region must fit inside the size that was actually
decoded. The walk is bounded by the address window capabilities live in and by a
hop counter, so a device offering a circular list terminates. This is the one
place in the area that reads a structure a device controls, and it is written as
if the device were hostile.

The walk also collects **shared-memory regions**, whose extents are 64-bit and
split into halves across the capability — so the containment check is written in
its non-wrapping form: the start is compared against the window size, then the
length against what remains after the start. The naive sum of two hostile halves
overflows, and a wrapped sum compares as small. A device offering more of these
than there are slots has the surplus silently ignored, which is fail-safe (the
driver sees fewer regions, never a wrong one) but is a silent truncation and not
signalled anywhere.

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

Two bump arenas for window addresses, low and high, each seeded lazily and
independently from the device tree; the high one is additionally marked *absent*
when the tree describes no such range, so the probe is not retried on every
allocation. Neither reclaims: a freed window's address is never handed out
again, and the argument is headroom — the low window alone is large enough for
tens of thousands of claims against a handful of live ones. Note that the
exclusivity table *does* reclaim, so a re-claim of the same hardware succeeds
and simply receives a fresh address.

The function object also carries a small fixed set of shared-memory region
records — identifier, window index, offset, length — filled from the capability
walk.

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

**[[inv-i34]]** — the allowance is the second axis on every create here: the
capability says *may you*, the allowance says *over what*. The two-step create
this file performs (check, then install under the revoke's lock) is that
invariant's central mechanism.

**[[inv-i32]]** — partially, and with a gap that is deliberate. A buffer's pages
are **not** charged to the creating Proc's page budget; the bound is the
allowance's per-buffer ceiling instead. That shape is not an oversight here but
a consequence of [[inv-i34]]'s data model, which carries a single maximum size
and so has nowhere to express a sum. Until a cumulative budget exists, the
per-Proc page accounting does not see DMA at all.

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
- **The two width rules are independent.** The size inversion must stay
  width-correct, *and* placement must keep refusing to put a 32-bit window in the
  high arena. Getting either right proves nothing about the other: one decodes a
  size, the other writes an address, and only the second can leave the device and
  the kernel decoding different memory.
- **Arena selection must test what remains, not what the window holds.** The
  fall-through to the high arena is reachable only if the low one reports itself
  full rather than merely large enough in principle.
- **Only the constructor may decide a buffer's class.** The mutual exclusivity of
  the class bits is enforced by the enumerated argument and nothing else; a
  consumer elsewhere disambiguates them by test order, so adding a class as
  another boolean, or a buffer that is legitimately two classes, silently
  re-kinds it there.
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
only bound — and there are now three ceilings rather than one, so the missing sum
spans more classes than when the gap was recorded. Neither bump arena reclaims.
The virtio transport slots are deliberately unreserved under a trust-boundary
argument with a stated expiry — see [[inv-i5]].

GPU buffer objects are allocated **at runtime, per client request**, where
framebuffers are minted once at startup — so they meet long-uptime allocator
fragmentation in a way the earlier class never did, at a ceiling that needs a
large contiguous run. The contiguity is this object's constraint and not the
device's: the hardware interface accepts a scattered list. Recorded in the header
as the follow-on if it bites.

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

Re-read 2026-08-16 at `efd83109` — `dma_handle.c` now 213, `pci_handle.c` now
561, `mmio_handle.c` unchanged. The GPU work landed the high arena, the placement
width rule, the arena availability test, the shared-memory capability, and the
buffer class enum. Cross-checked this pass: every writer of either class bit
across the kernel and architecture trees (one, the constructor), the class
readers in the network dataplane, and the shared-memory containment arithmetic.
[[chg-2026-08-16-hwcap-widths]].

Absorbed `docs/reference/39-hw-handles.md` and `docs/reference/115-pci-claim.md`.
