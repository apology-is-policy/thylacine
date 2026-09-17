---
id: sub-kernel-gic-msi
type: sub
parent: moc-kernel-devices
title: "GIC MSI domains and vector retirement"
code:
  - arch/arm64/gic_msi.c
  - arch/arm64/gic_msi.h
  - arch/arm64/gic_its.c
  - arch/arm64/gic_its.h
audit: hard
guarded-by: [inv-i5, inv-i15, inv-i18]
validated-by: [prose, gate-smp]
locks: [lock-gic-msi, lock-lpi-lease, lock-its-command]
abis: []
design: ["docs/PCI-INTERRUPTS-DESIGN.md"]
created: 2026-09-17
updated: 2026-09-17
---
## Purpose

Own MSI vectors independently of raw wired IRQ claims. The current foundation
supports GICv2m and GICv3 ITS/LPI allocation, retirement and mediated PCI MSI-X
delivery. Resident drivers select MSI-X on both backends. HVF Instrument desktop
workflows pass on both GICv2m/HVF and ITS/GICv3/TCG.

## Contract

Boot discovery reserves valid, enabled owning-GIC GICv2m SPI ranges against raw IRQ creation,
including unused vectors and frames that cannot be allocated. Allocation follows
the requesting BDF's DTB controller relationship and returns a masked route.
Message address and data remain kernel-only. A software generation identifies
the allocation; it cannot identify a late hardware message.

Retirement requires device masking, posted-write completion, removal from
endpoint membership and dispatch-pin drain. A proven quiescent source permits
controller pending-state cleanup and a bounded wait for active IRQ completion.
Without that proof, or on timeout, the vector is quarantined. A later proof may
reclaim that same lease; a stale lease cannot retire a replacement.

## Mechanism

GICv2m MSI_TYPER supplies the SPI base/count unless paired validated firmware
overrides exist. MSI_SETSPI_NS supplies the message address; the payload is the
INTID, with documented relative-SPI offsets for X-Gene and Broadcom NS2.
A usable frame must be a child of the initialized GIC distributor, validated
by its translated physical address. Frames overlapping another MSI frame, raw kernel ownership or PCI INTx routing
are not allocatable. Both frames become unusable on an overlap.

Permanent GIC callbacks resolve software ownership through the PCI endpoint
layer. No GIC argument points at an allocated lease. The callback masks an asserting endpoint entry and publishes a completion
ticket; unowned/stale vectors are masked at the controller.

The ITS backend follows the same kernel-derived DeviceID/EventID authority.
It prepares per-CPU collections and validated redistributor property/pending
tables before SMP. Routing policy currently targets CPU 0, as wired SPIs do.
Property bytes use a separate 64-LPI namespace beginning at INTID 8192. CPU
writes are cleaned to PoC; hardware-owned pending/device/ITT tables are never
written by the CPU after initialization. Register attributes are read back.

ITS commands use a 64KiB ring per controller with absolute producer/consumer
counters and one reserved empty slot. IRQ-safe property changes enqueue INV
without waiting. MAPD/MAPC/MAPTI and retirement wait for SYNC outside locks,
with a 100ms bound per batch. Retirement additionally requires a target-CPU IRQ
barrier: LPIs have no SPI active bit, so command completion alone cannot prove
that an already acknowledged handler has finished.

## Data structures

Eight permanent v2m frame records and an SPI-indexed vector registry. Up to four
ITS controllers have boot-lifetime command/device/collection/ITT allocations;
a separate 64-entry LPI lease registry tracks BUILDING/LIVE/RETIRING/quarantine.
Eight EventIDs per enumerated function are supported, with no requester aliases. The system
quota is 64 allocated or quarantined vectors. Generation exhaustion fails
closed. FREE, LIVE, RETIRING and QUARANTINED are distinct allocator states.

## Concurrency

[[lock-gic-msi]] protects allocation state and generations. RETIRING excludes
reuse while controller drain runs outside the lock. Firmware frame metadata
and reservations are immutable after boot, before SMP and EL0 start. ITS lease
and command ordering are [[lock-lpi-lease]] -> [[lock-its-command]]. Neither
lock is held across command completion or CPU synchronization.

## Invariants enforced

Raw IRQ handles never claim MSI-reserved vectors. Only a route derived from
firmware can be allocated. Device notifications cannot carry a software lease
generation, so software lifetime alone never authorizes vector reuse.

## Error paths

Unsupported controller, missing route, conflicting firmware, exhausted quota
or exhausted generation returns allocation failure. Drain has a 10ms deadline;
a failed drain consumes quarantine capacity rather than recycling the vector.

A command fault disables the ITS engine and retains uncertain DMA allocations.
A bounded atomic mailbox and permanent SGI 14 notify PCI endpoints after ITS
locks have been released. The PCI callback masks affected functions, faults
all associated endpoints and wakes blocked waiters. No controller fault callback
runs under an ITS lock.

## Performance

Allocation scans at most eight frames and the architectural SPI namespace.
Interrupt dispatch performs no allocation. No spinlock is held while waiting
for controller completion.

## Prosecution

`pci.msi_allocator` passes on GICv2/HVF: masked initial allocation, denial of raw
IRQ ownership, pending-state drain, reuse with a fresh generation, stale-lease
rejection, quarantine isolation, later reclamation and bounded exhaustion.
The real `pci.msix_rng` guest test submits entropy buffers to virtio-RNG,
proves DMA completion under the entry mask, then observes PBA delivery after
ARM and COMPLETE. It also checks replay and stale-ticket rejection without
reading the INTx ISR. The HVF boot passes all 1,562 kernel tests. Two resident
MSI-X Instrument workflows pass on HVF. Idle sound deliveries stayed at 173
over 46.23 seconds while GPU advanced 290 -> 519; retries/cooldowns stayed zero.
The latest GICv3/TCG boot passes 1,565 tests, including real RNG MSI-X/PBA
delivery, allocator retirement/quarantine/reuse, cross-CPU IRQ barriers,
command-ring wrap/saturation/stall/timeout and blocked-waiter fault fanout.
The actual ITS Instrument workflow passes in 173 seconds; sound deliveries
stayed at 187 while GPU advanced 223 -> 310 over eight seconds, without
retries or cooldowns. The full workload/controller matrix remains owed.

## Seams

[[sub-kernel-dtb]] owns controller/requester discovery.
[[sub-kernel-hwcap]] owns controller MMIO protection and PCI BAR isolation.
[[sub-kernel-pci-irq]] owns function endpoints and explicit completion tickets.
[[sub-kernel-gic]] owns SPI masking, pending/active state and EOI.

## Caveats

Kernel PCI table programming and reset-backed retirement are implemented.
Driver mode/vector selection is boot- and Instrument-verified on HVF and
GICv3/TCG. Asynchronous GPU completion-pump verification and the full
cross-mode/device-order/SMP matrix remain open.
Unsupported redistributor layouts or register attributes decline MSI-X. Partially
initialized controller tables are retained at boot rather than risking DMA into
freed memory. Command failure quarantines uncertain leases; no timed reuse.

## Provenance

Approved design: [[dec-2026-09-17-pci-interrupt-domains]].
