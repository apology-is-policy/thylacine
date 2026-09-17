---
id: sub-nocturned
type: sub
title: "Nocturne — device cadence, mixing and audio authority"
parent: moc-userspace
code: [usr/nocturned/src/main.rs, usr/nocturned/src/snd.rs, usr/nocturned/src/server.rs, usr/nocturned/Cargo.toml]
audit: hard
guarded-by: [inv-i5, inv-i9, inv-i34]
validated-by: [prose]
locks: []
abis: []
design: ["docs/NOCTURNE.md", "docs/PCI-INTERRUPTS-DESIGN.md"]
created: 2026-09-17
updated: 2026-09-17
---
## Purpose

Own a virtio-sound PCI function and serve PCM playback, voices and supported
capture through 9P. The public `/srv/nocturne` tree supplies normal playback;
`/srv/nocturne-ctl` carries the separately checked sink/capture authority.
The server checks peer identity and authority rather than trusting mount paths.
See `server.rs` for the node-specific admission checks and `docs/NOCTURNE.md`
for the protocol and graph contract.

## Contract

The server accepts bounded 9P requests and retains voice resources through fid
lifetime. The public mount supplies playback; sink control, tap and device
capture have distinct admission checks. `source` is available only when the
transport negotiated capture. Both tap and source permit one authorized reader
and use bounded drop-oldest mirrors rather than blocking the audio cycle.

## Mechanism

Three threads share one daemon process. `irq_entry` owns the IRQ handle and a
mode-aware acknowledgement accessor. `cycle_entry` owns `VirtioSnd`, its PCI mapping,
DMA pool and queue state. The serving thread owns the 9P connections. `Shared`
holds graph state and the cycle wake word; the cycle uses `try_lock` and can
replay the last period when a graph edit holds the lock. The control thread
does not hold the graph lock across a reply.

`PciIrq::for_virtio` selects and verifies one masked config/control/event/TX/RX
vector before DMA setup, preferring MSI-X with transactional INTx fallback.
The IRQ waiter is promoted by the kernel's IRQ wait. In INTx mode it reads ISR;
in MSI-X mode it issues the device barrier without consuming ISR. It
pokes the cycle before explicitly completing its PCI interrupt ticket.
WAIT never re-arms; EAGAIN completion leaves the source masked until a delayed
retry returns the same ticket. The first arm follows initialization ack. The cycle
parks with a bounded backstop, independently of a device that stops producing
interrupts. The IRQ accessor retains only the MMIO register address; the
non-returning cycle context retains the PCI mapping for the process lifetime.
MMIO barriers complete acknowledgement before re-arm. Queue indices, not ISR
notification counts, determine work to consume.

The transport bounds device-derived descriptor IDs, lengths and queue-drain
passes. Capture is absent when no compatible input stream is negotiated.
Playback and capture authority are separate from device discovery and normal
playback namespace visibility.

## Data structures

`VirtioSnd` owns the PCI function, DMA pool, negotiated stream flags, control/TX/
RX indices and device counters. `Graph` owns voices, sink policy and capture
mirrors. `Shared` combines its mutex with the cycle wake word. `Conn` retains
per-connection fids and pending requests. Resource ceilings are named in
`server.rs`: `MAX_FIDS`, `MAX_PENDING_WRITES` and `MAX_VOICES`; they are enforced
at admission rather than trusting client-provided identifiers.

## Concurrency

Only the cycle mutates queue state. Control and cycle serialize graph access;
the cycle never waits for the graph lock. The IRQ accessor touches a device
register independently of mutable Rust queue memory. `cycle_seen` precedes the
pump and `cycle_park_since` rechecks the wake value, preventing an intervening poke
from being absorbed as already-observed work. Kernel IRQ wait has exactly one
caller in the daemon.

## Invariants enforced

Hardware handles remain non-transferable ([[inv-i5]]) and Warden confines the
function/DMA authority ([[inv-i34]]). The register-before-check cycle handshake
preserves wakeup ordering ([[inv-i9]]). Namespace reachability is not audio
control authority: server admission uses the live peer snapshot. Device-supplied
indices never directly select unchecked memory.

## Error paths

Unsupported transport regions/features fail probe. An unavailable capture stream
leaves playback usable. Invalid device completions update error counters and are
not blindly reposted. An unexpected IRQ-wait error terminates the dedicated
waiter, while the cycle retains its timeout backstop. Partial server requests
and resource exhaustion return protocol errors; disconnect/clunk releases the
corresponding server ownership. These branches require the explicit audio and
authority gates, not just a successful Warden READY line.

## Performance

The device period is the playback clock. Inactivity stops playback after the
configured silence interval. Cycle/control backstops bound missed notifications;
no latency measurement is claimed by this dossier. An interrupt storm can
invalidate normal cadence assumptions even when a short audio sample succeeds.

## Prosecution

`tools/test-audio.sh`, `tools/test-ring-audio.sh` and `tools/test-sdl-audio.sh`
exercise audible-output data through captured WAVs. The volume, tap and capture
scripts exercise separate authority paths. A successful playback witness is
blind to an idle IRQ storm and to shared-line starvation outside its sample;
interrupt-rate and competing-workload tests are additionally required.

## Seams

[[sub-kernel-pci-irq]] now isolates function delivery on shared INTx lines.
The separate IRQ thread retains an endpoint/PCI reference while the cycle owns
queue memory. VirtioSnd Drop resets before DMA fields are destroyed; closing a
parent handle alone cannot quiesce a function retained by the endpoint.

## Caveats

Shared-INTx Instrument runs pass three times on HVF. MSI-X/GICv2m
Instrument runs pass twice on HVF. Idle audio counters remain flat during the
measured windows (10.56 seconds INTx/HVF; 46.23 seconds MSI-X/HVF), with no
retry/cooldown accumulation. Sustained audio/capture, ITS and the full mode/
device-order/SMP matrix remain owed. This work has not reached main.

## Provenance

(generated -- incoming `touched` backlinks)
