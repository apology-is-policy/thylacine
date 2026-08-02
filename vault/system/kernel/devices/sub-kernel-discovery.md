---
id: sub-kernel-discovery
type: sub
parent: moc-kernel-devices
title: "Enumerate once, republish forever"
code:
  - kernel/virtio.c
  - kernel/include/thylacine/virtio.h
  - kernel/virtio_pci.c
  - kernel/include/thylacine/virtio_pci.h
  - kernel/devhw.c
  - kernel/devpci.c
audit: light
guarded-by: [inv-i5, inv-i15, inv-i34]
validated-by: [prose, gate-smp]
locks: []
abis: []
design:
  - "docs/MENAGERIE.md section 7"
  - "docs/ARCHITECTURE.md section 9.4"
  - "docs/ARCHITECTURE.md section 22.7"
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Answer the question a driver process asks first: *what hardware is here?* The
kernel walks the two buses once, at boot, and then publishes what it found as
two read-only namespaces. Nothing in userspace ever touches a bus.

## Contract

Two enumerations and two trees.

- **`/hw`** is the device tree, republished. Each node is a directory, each
  property a file holding the property's **raw bytes, big-endian, exactly as on
  the wire**. The kernel does not interpret; the reader decodes.
- **`/hw/pci/<bus.dev.fn>/ctl`** is one text line per discovered PCI function:
  its address, its vendor and device ids, its derived virtio id, and its routed
  interrupt number — or `none`.

Both are read-only in the strongest sense: write, create, wstat, remove and
power all fail. Both are `perm_enforced = false` — **visibility, not
authority**. Seeing that a device exists is not a privilege; a driver cannot
bind without reading its registers and interrupts, and the device tree is not a
secret. The privilege boundary is the allowance ([[inv-i34]]), which gates
minting a handle over what you found, not looking.

## Mechanism

**Discovery is a projection, and what the projection hides is decided by
whether reading the source is itself an act.** This is the one idea that
explains why two Devs built to the same shape disclose such different amounts.

The device tree is inert data — a buffer the kernel relocated at boot and never
writes again. Reading it costs nothing and reveals nothing that matters, so
`/hw` hands back the bytes verbatim and lets the consumer parse. PCI config
space is the opposite: it is a live window onto the bus, where a *read* is a
transaction and a *write* reconfigures hardware. So `/hw/pci` publishes no
window at all. It republishes **conclusions**.

**The interrupt number is where that shows most sharply.** Building a `ctl`
line performs a real config-space read — the function's declared INTx pin — and
then swizzles it through the device tree's interrupt map. Only the resulting
GIC number crosses to userspace. The driver gets the answer and never the
mechanism, which is the whole content of "mediated": userspace never gets raw
ECAM, and there is no config-space write surface anywhere in the tree.

**Each namespace's identity is borrowed from its source's own addressing.** A
`/hw` qid *is* a byte offset into the flattened tree's structure block, with
one bit marking property-versus-node; the offsets are stable because the buffer
is immutable. A `/hw/pci` qid is an index with a parity bit — even is the
function's directory, odd is its `ctl` — so one shift recovers the index from
either. Neither Dev keeps a lookup table, because the source already is one.

**A sentinel lives inside each of those encodings, and is kept safe by
decoding order.** `/hw` carries a synthetic `pci` child — a mount point for the
PCI tree — marked by a bit no real offset can reach, and every operation tests
for it *before* the offset logic, so the sentinel is never handed to the tree
walker as a position. This is the second instance of the area's sentinel habit;
[[sub-kernel-hwcap]] holds the first, and both are safe for the same reason:
not because the value is unreachable, but because the code controls who decodes
it.

**Enumeration fails toward absent.** Every PCI config accessor returns
all-ones when it rejects a request — out of range, misaligned, no device — and
all-ones is precisely what the bus itself returns for *nothing here*. A refused
read is therefore indistinguishable from an empty slot, which is the correct
answer rather than a lucky one. The MMIO transport does the same differently:
an unreadable slot yields zero, which fails the magic-value check that gates
recording it at all.

**Neither enumeration trusts what it finds.** A tree entry advertising a
transport whose magic does not match is dropped rather than fatal. A queue
whose device-declared size is zero or not a power of two is refused outright,
because the ring's index arithmetic masks with that size and a non-power-of-two
would wrap unsoundly — a malformed device gets a closed door, not an armed
ring. Compile-time assertions pin the default queue size so that all three
rings fit the single page each is given.

**What the kernel can see is bounded by what it can map.** Only the first PCI
bus is mapped, one megabyte of config space, because mapping the whole range
would exhaust the window the kernel has for device memory. The machine may be
larger than the kernel's view of it; the view is bounded by an address-space
budget, not by the hardware.

**Teardown at a driver's death is a broadcast, with one carve-out.** When a
Proc holding device memory dies, every transport slot inside its range is
reset. The entropy source is skipped by device id: the kernel drives that one
itself, and a dying driver must not stop a device that was never its own.
Resetting is not enough on its own — disarming a queue does not drain a
transfer already in the device's pipeline, so anything returning ring pages to
the shared allocator must reset the whole device first, and the per-queue
disarm is only the second layer.

## Data structures

Two fixed arrays, both built once at boot: thirty-two MMIO transport slots and
sixteen PCI functions. A slot records its physical address, size, mapped kernel
address, version and device id. A function records its address on the bus, its
vendor, device, subsystem and class identifiers, whether it is a modern or
legacy device, its derived virtio id, and the kernel address of its four-kilobyte
config space.

Because the slots are half a kilobyte apart, most do not begin on a page
boundary; the mapping covers the containing page and the slot's address is
computed as an offset within it.

A queue is three independently allocated pages — descriptors, driver ring,
device ring — each registered with the device by physical address.

## Concurrency

**There are no locks in any of these four files.** Both tables are written
before secondary processors start and are read-only for the rest of the
machine's life; immutability is the synchronization. The one in-kernel consumer
of the MMIO transport carries its own lock, in its own file.

That immutability is not merely convenient — it is **load-bearing in three
places**. It is why a namespace index minted by a walk stays valid forever; why
looking a device up by identity and then claiming it are guaranteed to reach
the same function, which is what makes the allowance's two-step create sound
([[inv-i34]]); and why a qid can be a raw offset rather than a handle.

## Invariants enforced

**[[inv-i15]]** — `/hw` is this invariant's honest enforcement. The hardware
view derives from the device tree because the device tree is what userspace is
handed, unedited.

**[[inv-i5]]** — no raw config space and no config-space write reach userspace;
only bounded, derived topology. This file also supplies the *mechanical* reason
the transport slots are exempt from pre-claiming: reservation works at page
granularity and the slots are packed eight to a page, so a driver claiming its
own slot necessarily claims seven neighbours. The exemption is structural, not
merely a matter of API convenience.

**[[inv-i34]]** — negatively, and deliberately: both trees are readable by
anyone who can reach them, because they confer nothing. The gate is on minting
a handle over what you read.

## Error paths

Returning nothing and continuing: a slot whose magic does not match; a tree
entry with an empty range; a queue whose declared size is zero or not a power of
two; a full table (the enumeration truncates rather than stopping the machine —
an operator may legitimately have more devices than storage).

Returning failure to the caller: a byte read of a directory; an enumeration of a
file; a walk from a leaf; a stat of a qid naming no function; a first directory
entry too large for the caller's buffer — which must be an error rather than
zero, because zero already means end-of-directory.

Ending the world: initializing either enumeration twice, and failing to map PCI
config space when the tree said it exists.

## Performance

Enumeration is one-shot at boot: thirty-two slots probed, and every function on
the first bus. Afterwards a walk is a linear scan of at most sixteen functions
and a `ctl` read rebuilds a sixty-three-byte line into a ninety-six-byte buffer.
Nothing here is on a hot path.

## Prosecution

- **The mediation boundary is the whole point.** Any future write surface, or
  any exposure of a config-space window rather than a derived value, breaks the
  property that userspace never pokes the bus.
- The synthetic mount-point sentinel must keep being tested before the offset
  decode. It is a value inside the space of real offsets, made safe only by
  ordering.
- Both tables must stay immutable after boot. A mutable device table would
  reopen the check-one-function-claim-another race the allowance's two-step
  create exists to close, and would invalidate every minted qid index.
- The queue-size rejection must stay fail-closed. It is the only thing standing
  between a hostile declared size and unsound ring arithmetic.
- The directory cursors must stay strictly increasing and never zero. `/hw`
  gets this free from the tree's own layout — a node's body begins past its own
  opening token — rather than by construction, so a change of cursor basis must
  re-establish it.
- **Non-seekability is load-bearing, not incidental.** A directory cursor here
  is a raw structure-block offset; allowing a seek would let it be aimed into
  the middle of a token and made to misparse.
- The death-time reset must keep skipping the kernel-driven entropy source, and
  anything freeing ring pages to the shared allocator must reset the device
  rather than relying on the queue disarm.

## Seams

Only the first PCI bus is enumerated; bridges and secondary buses are not
walked. The high half of the feature space is negotiated as zero rather than
supported. Message-signalled interrupts are not enabled — interrupt delivery is
the legacy pin route. A full table truncates the enumeration silently (below).

## Caveats

- **The reported interrupt and the claimed interrupt are computed
  differently.** The `ctl` line routes the pin the function *declares*; the
  claim path ([[sub-kernel-hwcap]]) hardcodes the first pin instead. The route
  depends on the pin — that is what the swizzle is for — so for any function
  declaring a different pin the two disagree, and for a function declaring no
  interrupt at all the claim can still mint a binding the device never asked
  for. Verified: the divergence, and that the route depends on the pin. Not
  verified: which pins the current devices declare — believed uniform, which is
  why this is dormant rather than live. This is the same shape as the
  duplicated page-order helper next door: two independently maintained answers
  to one question, agreeing today by accident of configuration.
- **A truncated PCI enumeration is not surfaced.** The code comments say the
  overflow is logged so the boot banner can report it; neither happens, and the
  banner prints a count that is simply capped. The sibling MMIO enumeration
  *does* report what it skipped. So the one path that can silently under-report
  the machine is the one whose comment promises it cannot.
- A `ctl` file's size is obtained by building the whole line and measuring it;
  there is no stored representation. Reads and stats each rebuild it, including
  the config-space read.
- The two sibling Devs disagree on a negative directory cursor: one treats it as
  the start, the other refuses. Both are safe — the cursor comes from the
  syscall layer — but they were written to the same pattern and drifted.
- One header is included for a constant the file explicitly does not use; its
  own comment says so. Vestigial.
- The kernel's only in-tree consumer of the MMIO transport is the entropy
  source. Everything else that drives one of these devices is a userspace
  driver, which is why the transport core's main in-kernel job is enumeration
  and death-time quiescing rather than I/O.

## Provenance

Read from `kernel/virtio.c` (351), `kernel/virtio_pci.c` (305),
`kernel/devpci.c` (497), `kernel/devhw.c` (404) and the two transport headers,
2026-08-02, at `631c8ade`. Cross-checked: the interrupt-map swizzle in
`lib/dtb.c`, the claim path's routing call, the boot mount order in
`kernel/joey.c`, the death-time reset callers in `kernel/proc.c`, the entropy
source's transport use, and the thirty-seven registered tests across the four
files.
