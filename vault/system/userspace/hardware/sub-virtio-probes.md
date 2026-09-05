---
id: sub-virtio-probes
type: sub
title: "The virtio reference drivers — where each device class was made to work"
parent: moc-userspace-hardware
code:
  - usr/virtio-blk-rw/src/main.rs
  - usr/virtio-blk-rw/Cargo.toml
  - usr/virtio-net-arp/src/main.rs
  - usr/virtio-net-arp/Cargo.toml
  - usr/virtio-net-loop/src/main.rs
  - usr/virtio-net-loop/Cargo.toml
  - usr/virtio-input/src/main.rs
  - usr/virtio-input/Cargo.toml
  - usr/virtio-gpu/src/main.rs
  - usr/virtio-gpu/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

Five standalone drivers, one per virtio device class, each written against the
raw hardware-capability syscalls rather than the driver framework. Block
(`virtio-blk-rw`), network in two stages (`virtio-net-arp`, then
`virtio-net-loop`), input (`virtio-input`), and GPU (`virtio-gpu`). Together
they are the only end-to-end exercise in the tree of the MMIO / DMA / IRQ
capability triple as a *program* uses it, and none of them is how the
corresponding device is driven in production.

Each was the chunk that made a device class work for the first time, and each
survives as an executable proof that it still does. They are not tests in the
usual sense and not drivers in the usual sense: they are complete drivers whose
only consumer is the assertion that they ran.

That gives them a double role, and both halves are load-bearing:

- **As proofs.** The in-kernel test suite spawns each one and grades it by exit
  status. A regression in the capability syscalls, the DMA coherence story, or
  the IRQ delivery path fails a boot rather than waiting for a production
  driver to be written against the broken surface.
- **As references.** Two production drivers were derived from members of this
  set. Stratum's block backend is a port of `virtio-blk-rw`
  ([[sub-stratum-bdev]] says so directly), and [[sub-netdev]]'s MMIO transport
  is the network pair generalized into a library. When a third device class
  needs a driver, this is what gets read.

The second role is the one that makes their internal quality matter more than
their runtime footprint suggests. A comment here does not merely describe this
program; it teaches the next one.

## Contract

Each is spawned with no arguments, no stdio, and broad hardware authority; each
returns an exit status the kernel test asserts on. The status vocabulary is
uniform and is the whole interface:

| Status | Meaning |
|---|---|
| 0 | the proof ran and passed, **or** the device is absent and the program skipped |
| non-zero | the device was present and something did not hold |

The collapse of PASS and SKIP into one value is deliberate. A machine without a
GPU, or booted without a network backend, must not fail the suite — so each
program probes for its device first and exits 0 with a `SKIP` line if it is not
there. The distinction survives only in the console text, which is why the
harness greps for specific strings rather than trusting the status alone:
[[sub-substrate-gates]] keys on `virtio-input: SKIP` for exactly this reason.

Diagnostics go to the console directly rather than to stdout. These programs are
spawned without file descriptors, so a write to fd 1 would go nowhere; the
console call is the only channel that survives.

## Mechanism

All five follow one sequence, which is the substrate the whole set exists to
prove:

1. **Claim the MMIO bank.** Every member maps the entire QEMU-virt virtio-mmio
   bank — sixteen pages, claimed one page at a time — at a fixed user VA. The
   bank base is a hardcoded constant, not a DTB lookup.
2. **Scan for the device.** Walk the thirty-two transport slots, read each
   slot's magic and DeviceID register, and take the first slot matching the
   class this program drives. Absent means SKIP.
3. **Run the VIRTIO 1.2 initialization handshake.** RESET, ACKNOWLEDGE, DRIVER,
   feature negotiation with a `VIRTIO_F_VERSION_1` requirement, FEATURES_OK with
   a readback check, queue configuration, then DRIVER_OK last — so DRIVER_OK
   arms a device whose queues are already wired.
4. **Allocate DMA and publish descriptors.** One region for the rings and, where
   the class needs it, a second for payload. The physical address returned by
   the mapping call is what goes into the descriptor tables.
5. **Drive the device and drain the used ring**, waiting on the IRQ handle
   between batches.

The variation between members is what each one proves beyond the substrate:
`virtio-net-arp` a single round trip, `virtio-net-loop` the same mechanics
sustained past a full queue's worth so that descriptor recycling and index
wraparound are exercised, `virtio-blk-rw` a read-verify-write-readback cycle
scaled to the disk, `virtio-input` selector-based config space and an RX-only
queue, `virtio-gpu` a five-command chain through a control queue.

## Data structures

There are no interesting structures — that is itself the point. Every virtqueue
is laid out by hand at fixed byte offsets inside a DMA region, and the code
reads and writes it through volatile accessors at computed addresses. There is
no descriptor struct; there is a 64-bit write at offset 0, a 32-bit write at
offset 8, a 16-bit write at offset 12.

The one structural discipline is a compile-time layout check. `virtio-net-loop`
carries a `const` block asserting that its six ring regions and its buffer pool
are in monotonic non-overlapping order and that the pool fits the DMA region —
so a future edit that grows a queue and silently overlaps two rings fails the
build rather than corrupting at runtime. This is the local instance of the
project-wide compile-time-invariant rule.

## Concurrency

Single-threaded, and the only concurrent party is the device itself. That makes
the memory-ordering discipline the whole of the concurrency story, and it
appears in two forms:

- **A full barrier before a doorbell.** Descriptor and buffer writes must be
  visible to the device before the index update that publishes them, and the
  index before the notify register write.
- **A read barrier after observing the used index.** Every drain reads the used
  index, then issues the barrier, then reads the used-ring entry and the buffer
  it names. Without it an out-of-order core may speculate the data reads ahead
  of the index load and return pre-advance bytes. All five carry this, and the
  comments explaining it cite the specification section — including
  `virtio-blk-rw` and `virtio-gpu`, whose failure mode would be misreading a
  zeroed response as a hardware fault rather than as a not-yet-written one.

`virtio-net-loop` also carries the drain cap that later became library code: a
ceiling on how many used entries one batch may consume, justified in its comment
as defense against a driver that relaxes back-pressure rather than as anything
needed today. [[sub-netdev]] has the same cap for the same stated reason.

## Invariants enforced

None. These programs enforce nothing on anyone's behalf — they *rely* on the
kernel's guarantees and demonstrate that the guarantees hold.

The one they demonstrate most directly is [[inv-i5]]: each acquires MMIO, DMA
and IRQ authority through the capability syscalls, and the resulting handles
cannot be transferred or duplicated. That is not checked here; it is the
kernel's property, and these are the programs that exercise the path where it
matters.

Worth stating explicitly because the file names invite the opposite reading:
they run with **broad** hardware authority. The kernel test spawns them with the
hardware-create capability and no allowance narrowing, so nothing bounds which
physical addresses they may claim — which is why each simply hardcodes the bank
base. They are inside the trusted tier by construction. The narrowed-allowance
story belongs to [[sub-menagerie-leaves]], and comparing the two is the clearest
illustration of what [[inv-i34]] added.

## Error paths

Every failure is a console line plus a non-zero exit, and the console line is
the diagnosis — there is no other channel. The classification is consistent:

- **Device absent** → `SKIP` and exit 0. Checked before anything is written to
  hardware.
- **Substrate refused** (a capability create or map returning an error) → a line
  naming which call and which page or region, then exit 1.
- **Device disagreed** (features rejected, a queue too small, a legacy version,
  a bad response code) → a line naming the observed value, then exit 1. Several
  of these first write the FAILED status bit so the device is left knowing the
  driver gave up.
- **Proof did not complete** → counters printed alongside the failure, so a
  partial run is legible from the log: how many were sent, how many completed,
  how many validated.

The gap in this scheme is a device that is present and silent, which is the
subject of the second caveat below.

## Performance

Not performance-sensitive and deliberately sized down. `virtio-net-loop` runs
twenty-four round trips through sixteen descriptors — the smallest ratio that
forces wraparound on both rings — rather than a load figure. `virtio-blk-rw`
scales its passes to the disk with a ceiling, so a large image does not make the
boot long.

The one number chosen against measurement rather than convenience is
`virtio-input`'s three-second poll budget, which exists because a window sized on
the emulated substrate proved far too short on the accelerated one when the
host-side helper was under load.

## Prosecution

The pointed questions for this set, in the order they pay off:

- **Does every device-supplied value get bounded before it is used, at its
  original width?** The used-ring descriptor id is the sharp case: it is chosen
  by the device, and it indexes a buffer pool. Three members read it, and the
  third caveat below is that they do not agree on how.
- **Does a device-reported length get clamped before a copy?** All five bound
  their reads by construction — fixed-size records, or a length compared against
  the minimum frame before any offset is computed.
- **Can any wait fail to terminate?** The answer is no for one member and yes for
  four; see the second caveat.
- **Does a failure leave the device armed?** Several failure paths write the
  FAILED status bit; not all do. Since the process exits and the kernel releases
  the claims, a still-armed device with freed rings is the same hazard
  [[sub-netdev]] documents — bounded here only because these run before any other
  consumer of that bank exists.

## Seams

- **The bank claim is exclusive and whole.** Each member claims all sixteen pages
  of the virtio-mmio bank, which means no two of them can run concurrently, and
  none can run while anything else holds a slot in that bank. Sequential
  execution inside the kernel test suite, before the filesystem server exists, is
  what makes this work. It is the same page-granularity consequence recorded
  against the framework path in [[sub-menagerie-leaves]], reached from the other
  direction.
- **The bank base is hardcoded**, so these are QEMU-virt programs. A different
  board moves the constant. The framework path solved this — a bus source
  enumerates and the warden grants — and these predate it.
- **`virtio-net-arp` leaks its RX buffer** by design: it is single-shot and exits,
  so the consumed descriptor is never recycled. Documented in place, and correct
  for what it is; `virtio-net-loop` exists because that shortcut does not scale.

## Caveats

- **The reference role is not stated in any of them.** Each header describes what
  its chunk proved; none says "a production driver was ported from this file",
  which is true of at least two. The consequence is asymmetric quality control:
  the derived copies have been audited as production code and hardened
  accordingly, while the originals were reviewed as probes. The next two caveats
  are both instances of that gap.
- **Four of five would block forever on a device that goes silent, and two of
  those describe an iteration counter as protection against it** (task #146).
  The IRQ wait has no timeout. `virtio-input` cannot hang — its interrupt is
  pre-fired by the spawning test and its subsequent loop is bounded by a
  wall-clock budget with an unconditional iteration backstop, a shape it acquired
  from two named bugs. `virtio-blk-rw` and `virtio-gpu` bound *spurious wakes*
  and say exactly that, which is honest. `virtio-net-arp` and `virtio-net-loop`
  wrap the blocking wait in a counted loop and call the count a defensive ceiling
  — but a counter outside a blocking call bounds how often the loop completes,
  never how long one pass may take, so it cannot fire in the case its comment
  names. Since these are graded by a kernel test that reaps them, a hang is a
  boot hang: the full harness timeout with no diagnosis.
- **`virtio-net-loop` narrows the device's descriptor id before bounding it**
  (task #147). It reads the used-ring id as a 32-bit value and truncates it to 16
  bits in the same expression, so the guard downstream — which is correctly
  written and carries a comment about the out-of-bounds read it prevents —
  validates a value that has already been folded into range. Both siblings that
  face the same field bound the full width, and `virtio-input`'s comment names
  this exact mistake while citing `virtio-net-loop` as the pattern it mirrors.
  The damage is confined: a frame parsed from the wrong buffer, and a
  possibly-live descriptor re-published — the double-post that [[sub-netdev]]
  refuses, citing an audit finding. No out-of-bounds access, nothing crossing a
  process boundary, and not reachable against a device that does not lie.
- **The console is the only diagnostic channel and it is unstructured.** A
  failing run prints prose and counters; the harness greps for phrases. That is
  adequate while the phrases are stable and invisible when they drift.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
