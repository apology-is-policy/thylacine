---
id: sub-kernel-pci-irq
type: sub
parent: moc-kernel-devices
title: "PCI interrupt domains: function ownership on a shared wire"
code:
  - kernel/pci_irq.c
  - kernel/include/thylacine/pci_irq.h
  - kernel/test/test_pci_irq.c
audit: hard
guarded-by: [inv-i5, inv-i9, inv-i15, inv-i34]
validated-by: [prose, gate-smp]
locks: [lock-pci-irq-domain, lock-rendez, lock-pci-config]
abis: [abi-pci-irq]
design: ["docs/PCI-INTERRUPTS-DESIGN.md"]
created: 2026-09-17
updated: 2026-09-17
---
## Purpose

Deliver a PCI function's events without giving it ownership of the shared GIC
line or requiring its peers to acknowledge on its behalf.

## Contract

CREATE takes a writable owned PCI handle. An IRQ endpoint starts DISARMED;
ARM grants initial delivery, WAIT returns a generation/sequence ticket, and
COMPLETE re-arms only after acknowledgement. WAIT never re-arms and replays the
current ticket until completion, so copy-out failure cannot strand a source.
DISABLE is terminal. Raw IRQ creation rejects DTB-routed PCI lines even when no
endpoint exists. The endpoint is the PCI source kind of non-transferable KObj_IRQ.

## Mechanism

Enumeration disables unclaimed supported PCI functions. Claims are POLLED until
an endpoint arms them. The permanent shared-line callback scans at most 32
members, tests PCI Interrupt Status, disables each asserting function via its
own Command register, and publishes a ticket. It re-enables the shared line
before waiting for any driver. A stalled member stays disabled while its peers
remain serviceable. COMPLETE rejects foreign/stale tickets and leaves an
asserting function masked with EAGAIN and a delayed retry.

## Data structures

PciIrq holds a strong PCI reference, generation, sequence, state, diagnostics,
and dispatch-pin count. Domain membership is weak and bounded. GIC callbacks
point at permanent domains rather than freed endpoints. The event and info
records are owned by [[abi-pci-irq]].

## Concurrency

[[lock-pci-irq-domain]] precedes rendez.lock and [[lock-pci-config]]. WAIT takes
only rendez.lock and claims a single-waiter guard before sleeping. Dispatch pins
are taken under membership lock; wakeups happen after releasing it. Last unref
removes membership and drains pins before freeing. Parent quiescence sets its
terminal flag under config lock, drops that lock, then revokes endpoints; it
never acquires the domain lock while holding config lock. ARM/COMPLETE refuse
to re-enable a terminally quiesced parent.

## Invariants enforced

Function-derived authority; no raw INTID authority from a routing hint; no
ref-zero resurrection; no wake on freed memory; single-waiter busy refusal;
sequence exhaustion faults rather than wrapping; 16-bit Command writes preserve
adjacent W1C Status. A pending peer does not block a correctly acknowledged one.

## Error paths

Unavailable MSI-X currently returns ENODEV. Bad mode/ordinal/ticket is EINVAL;
second waiter or duplicate function endpoint is EBUSY; terminal revoke is
ECANCELED. Still-asserted COMPLETE is EAGAIN and supplies the same ticket after
100us through WAIT. Thirty-two unexplained domain arrivals fault its endpoints
and mask the line. A fresh endpoint may recover it only after all old endpoints
close, a 100ms cooldown, and a clear pending/active controller probe; failed
probes remain masked and restart the cooldown. Recovery never revives old tickets.

## Performance

No IRQ-context allocation. Membership is capped at 32. Initial policy delays an
event for 1ms after 128 deliveries/function/ms; counters expose delivery, retry
and cooldown counts. These thresholds need workload measurement before claiming
latency bounds. WAIT blocks through retry/cooldown instead of re-arming a storm.

## Prosecution

Guest shared-ticket test passes: two asserting functions, stalled peer isolation,
foreign/stale tickets, replay, timed retry, busy guard, terminal disable and
owner-death revoke. The 64-round last-close/dispatch test concurrently frees
one endpoint while a live peer receives and completes every assertion; it
checks parent reference release and IRQ live-count balance. It passes on both
HVF and TCG (1,568 kernel tests), alongside recovery and forced benchmark-child
failure controls. Real desktop/media tests pass on shared-a and shared-b PCI
layouts, with the no-MSI case routing NIC/GPU/audio onto one INTx line.
The remaining audio/controller and full SMP matrix checks are in progress.
This is single-agent self-review, not an independent audit.

## Seams

[[sub-kernel-hwcap]] owns config locking, quiescent claims and mappings;
[[sub-kernel-irqfwd]] delegates PCI endpoint destruction and rejects raw PCI lines;
[[sub-kernel-death]] finds owners via handles and mappings. Drivers use the
explicit ticket API in [[sub-libthyla-rs]].

## Caveats

Shared INTx and MSI-X through GICv2m and ITS/LPI are implemented. A faulted
physical INTx domain stays masked until all old endpoints close, a 100ms
cooldown expires and a non-waiting controller probe observes no pending or
active interrupt. Failed probes restart the cooldown. Recovery creates a fresh
endpoint; it never revives old tickets. `pci.intx_recovery` passes on both HVF and TCG, including cooldown survival
after last close, refusal while old endpoints remain, and fresh delivery after
recovery. The full mode/device-order/SMP verification matrix remains open. Repeated HVF Instrument
runs and one actual ITS/TCG run provide rate evidence, not whole-matrix proof.

## Provenance


## MSI-X endpoint lifecycle

MSI-X endpoints allocate a route from [[sub-kernel-gic-msi]], program and
read back a masked private table entry, then publish the handle. Up to eight
entries per function fit within the 32-endpoint system bound. All entries are
configured before ARM; an in-progress setup blocks a peer's ARM. Controller
operations run outside the endpoint spinlock. Parent revoke races converge on
terminal state and discard the unpublished endpoint.

Delivery masks the entry and publishes a generation/sequence ticket. A message
already in flight may increment the same event's saturating count. COMPLETE
unmasks the entry; PBA retains notifications during masking. MSI-X never reads
virtio ISR. Last endpoint close disables MSI-X for a later INTx initialization.
Routing leases survive uncertain close in hardware backing; bounded virtio
reset and controller drain permit reclamation during terminal function quiesce
or a later function claim. Readback failure retains the lease for quarantine.

The live virtio-RNG test covers masked-PBA delivery, replay and ticket freshness
on GICv2m/HVF and ITS/GICv3/TCG. Resident network, sound and GPU drivers
select MSI-X through the shared helper.

Live table readback failure faults the whole function, masks function delivery
and wakes every vector waiter with EIO. Controller failure faults all endpoints
on that controller through deferred notification. The fault flag is distinct
from owner revocation: ordinary BAR/DMA ownership remains until reset/teardown.
New endpoints are rejected until a fresh claim/reset. `pci.msix_failures`
checks program rollback/quarantine, arm and dispatch mask failures, controller
fault fanout and blocked-waiter wakeup.
