---
id: sub-lictor
type: sub
title: "Lictor - the trusted graphical seat and normal hardware broker"
parent: moc-userspace
code: [usr/lictor, kernel/include/thylacine/seat.h, usr/caps-probe]
audit: hard
guarded-by: [inv-i1, inv-i2, inv-i5, inv-i9, inv-i27, inv-i34, inv-i40, inv-i45]
validated-by: [prose]
locks: []
hazards: [haz-driver-panic-dos]
abis: [abi-trusted-seat, abi-native-nonblock]
design: ["docs/GRAPHICAL-SAK-OWNERSHIP.md", "docs/GRAPHICAL-SAK-PORTABILITY.md", "docs/HALCYON-TRUSTED-EPISODE.md"]
created: 2026-09-18
updated: 2026-09-18
---
## Purpose

Lictor owns the graphical seat's physical input and display transport. It is
part of the trusted computing base together with the kernel and [[sub-corvus]].
[[sub-tapestryd]], Halcyon, Beacon, application surfaces and ordinary composition
remain outside that boundary. The first backend is virtio GPU/input on QEMU;
Pi 400 and Pi 500 require their own controller and DMA isolation qualification.

## Contract

### Boot authority

[[sub-warden]] starts Lictor and Tapestry as separate sandboxed leaves. Lictor
receives the graphics/input PCI functions and the non-inherited seat-service
role. Tapestry receives only a DMA allowance and the normal seat-client role.
The kernel binds actual process incarnations, never a claimed name or PID alone.
The general prohibition on narrowed hardware drivers spawning children remains.

`SEAT_CLIENT` lets only the bound hardware service obtain the currently designated
compositor's PID and stripes. Each accepted `/srv/lictor` connection compares
both with `SYS_SRV_PEER`'s live kernel snapshot. Other processes cannot acquire
broker authority by opening the service or by holding `CAP_HW_CREATE`.
The normal designation itself confers no trusted endpoint operations.

## Mechanism

### Normal broker

The bounded 9P tree contains `ctl` and `rings/<resource>`. Control messages carry
strictly increasing connection sequences and bounded, completely decoded typed
requests. Unsupported 9P extensions return ENOSYS so the kernel can negotiate its
normal fallback. Filesystem mode bits permit operations inside an already
authenticated connection; the process-incarnation gate controls admission.

The service resolves normal resource/context IDs through its owner ledger.
Trusted IDs occupy a separate reserved range. Imported backing names a one-shot
share and logical byte range, never caller-supplied physical addresses.
`SYS_SEAT_IMPORT` claims only a share registered by the accepted connection's
actual peer, pins a weave/GPU-BO and returns kernel-derived scatter segments.
Pins survive ambiguous device completion and deferred retirement. Context links
must be gone and device retirement acknowledged before IDs or backing are freed.

The GPU command machinery remains in the trusted service. Its synchronous and
asynchronous commands share one monotone fence sequence. Synchronous responses
must return the requested fence; queue acceptance alone is insufficient for
trusted takeover. Legacy Virgl callbacks bound the sequence to 32 bits; exhaustion
fails closed rather than wrapping or using an incompatible high-bit namespace.

### Episode

Physical Ctrl-Alt-Delete starts a kernel generation. Normal hardware requests
park while the service drains all outstanding work, excludes every advertised
scanout and selects private, non-shareable trusted backing. Held keys and buttons
must be released before Corvus can receive input. No ordinary cursor commands
are exposed by this backend.

Corvus sends bounded semantic frames containing identity, exact capabilities,
term and verdict. Lictor uses baked fonts and colors. Corvus receives key bytes
only after Lictor acknowledges the current frame's actual presentation. The
ordinary compositor receives neither those bytes nor the trusted framebuffer.
The current backend uses the approved neutral backdrop when a completed private
workspace snapshot is unavailable.

END requires restoration and release drainage before normal admission resumes.
Tapestry observes the changed generation, releases previously held keys to their
original owners and schedules a full repaint. Failure leaves authorization closed;
owner death, stale generation, timeout or failed device completion cannot become
a successful graphical grant. A failed episode cancels its still-unredeemed
pending grant by exact target incarnation and session. Grants are held against early redemption until the RESTORED commit.
Redeemed grants then use the existing legate lifecycle.

## Data structures

The service owns the GPU transport, private Screen DMA and bounded normal object
ledger. Its CPU raster workspace is a separate private anonymous mapping,
committed at startup and reused for every frame. It never competes with normal
broker allocations in the general heap; restoration erases both raster and DMA
pixels. Geometry bounds cap each buffer at 4096 by 2160 pixels. Broker connections carry one sequenced transaction, a bounded fid table
and partial input/output buffers. Kernel seat state holds process incarnations,
generation, phase, semantic frame/visibility sequence, held keys and secret FIFO.

## Concurrency

The event loop polls physical input independently of broker reply progress.
Accepted endpoints are nonblocking, so a stalled normal reader cannot park the
trusted pump. Kernel transitions serialize under the process-table lock;
grant commit/cancellation nests the grant lock in that order. Imported backing
pins and host-memory extents survive uncertain device retirement. A host-memory
address returns to its allocator only after acknowledged unmap/unref and absence
of external mapping or claim references.

## Invariants enforced

Boot roles and peer-incarnation checks enforce [[inv-i2]] and [[inv-i34]].
Generation, physical input, visibility and held grants enforce [[inv-i27]].
Ownership ledgers, actual fence retirement and backing pins preserve [[inv-i40]]
and [[inv-i45]]. See the linked ABI contracts for bounded native messages.

## Error paths

Bad peers, reserved IDs, malformed sequences, oversized frames and unsupported
operations are refused. Transport EAGAIN retains partial progress. GPU ambiguity
retains reservations instead of freeing potentially live backing. Failed takeover,
owner death and stale/expired generations disable authorization. Restart must
re-establish hardware ownership; no ordinary client inherits the seat.

## Performance

Normal rendering adds a bounded broker round trip. Trusted scenes use CPU
rasterization into private DMA; the backend uploads complete frames on semantic
or mask changes. The neutral background avoids unqualified capture operations.

## Prosecution

QEMU graphical regression passes empty, confer, actual DAC elevation, abdicate,
wrong-key and Escape paths. Ordinary test descendants are refused trusted-seat
operations and GPU claims at every stage. The run disables serial authorization.
Broader accelerated/backend and SMP verification remains in progress. Host tests exercise framing,
semantic validation, ownership/retirement, keymaps and rendering rejection.
Kernel tests exercise seat role gates, physical-input admission, generation,
visibility acknowledgement, held-key restoration and pending-grant cancellation.
These tests are blind to actual scanout/input timing and backend DMA isolation;
QEMU end-to-end qualification and real screenshots are separate evidence. No
Pi hardware qualification is claimed.

## Seams

[[sub-warden]] supplies boot grants, [[sub-tapestryd]] supplies normal rendering,
[[sub-corvus]] supplies policy and semantic frames. [[abi-trusted-seat]] and
[[abi-native-nonblock]] pin the native boundaries. `caps-probe --seat-*` is an
ordinary guest test client, never a source of trusted authority.

## Caveats

The current backend is QEMU virtio GPU/input with a neutral backdrop. Pi DMA,
controller ownership and display conformance require separate qualification.
The physical hypervisor and host GPU renderer remain trusted for a VM. Review is
single-agent self-review by operator direction, not an independent audit.

## Provenance

Architecture and portability decisions are recorded in
[[dec-2026-09-18-graphical-sak-portability]]. Runtime evidence is tracked by the
integration verification record when the change lands.
