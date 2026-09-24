---
id: sub-lictor
type: sub
title: "Lictor - the trusted graphical seat and normal hardware broker"
parent: moc-userspace
code:
  - usr/caps-probe/Cargo.toml
  - usr/caps-probe/src/main.rs
  - usr/lictor/Cargo.toml
  - usr/lictor/src/backend/device.rs
  - usr/lictor/src/backend/gpu.rs
  - usr/lictor/src/backend/gpu/cursor.rs
  - usr/lictor/src/cursor_queue.rs
  - usr/lictor/src/backend/import.rs
  - usr/lictor/src/backend/input.rs
  - usr/lictor/src/backend/mod.rs
  - usr/lictor/src/backend/screen.rs
  - usr/lictor/src/backend/seat.rs
  - usr/lictor/src/backend/server.rs
  - usr/lictor/src/endpoint.rs
  - usr/lictor/src/fence.rs
  - usr/lictor/src/framing.rs
  - usr/lictor/src/gpu_api.rs
  - usr/lictor/src/keymap.rs
  - usr/lictor/src/lib.rs
  - usr/lictor/src/limits.rs
  - usr/lictor/src/main.rs
  - usr/lictor/src/model.rs
  - usr/lictor/src/objects.rs
  - usr/lictor/src/proxy/gpu.rs
  - usr/lictor/src/proxy/input.rs
  - usr/lictor/src/proxy/mod.rs
  - usr/lictor/src/render.rs
  - usr/lictor/src/typography.rs
  - usr/lictor/src/type_metrics.rs
  - usr/lictor/src/type.bin
  - usr/lictor/examples/render-preview.rs
  - tools/bake-lictor.py
  - tools/interactive/ls-graphical-sak.exp
  - tools/interactive/ls-graphical-sak-states.exp
  - tools/interactive/ls-graphical-sak-recover.exp
  - usr/lictor/src/rpc_client.rs
  - usr/lictor/src/skein.rs
  - usr/lictor/src/wire.rs
  - kernel/include/thylacine/seat.h
audit: hard
guarded-by: [inv-i1, inv-i2, inv-i5, inv-i9, inv-i27, inv-i34, inv-i40, inv-i45]
validated-by: [prose]
locks: []
hazards: [haz-driver-panic-dos]
abis: [abi-trusted-seat, abi-native-nonblock]
design: ["docs/GRAPHICAL-SAK-OWNERSHIP.md", "docs/GRAPHICAL-SAK-PORTABILITY.md", "docs/HALCYON-TRUSTED-EPISODE.md"]
created: 2026-09-18
updated: 2026-09-24
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

Physical Ctrl-Alt-F10 (with Ctrl-Alt-Delete retained for compatibility)
starts a kernel generation (the kernel scans the chord; [[abi-trusted-seat]]). Normal hardware requests
park while the service drains all outstanding work, excludes every advertised
scanout and selects private, non-shareable trusted backing. Held keys and buttons
must be released before Corvus can receive input. Operation 65 admits only
standard normal pointer shapes, scale and position from the authenticated
compositor. Private cursor DMA never enters the normal object ledger. Before
trusted scanout every cursor plane receives a transparent image followed by a
hide, with each queue retirement checked. The transparent replacement also
clears display listeners that ignore visibility updates. A failed cursor queue
retains its backing until GPU reset and prevents trusted acknowledgement.
Cursor waits share controlq's sticky allowance for an outstanding GPU readback;
the two queues share the device loop, so a delayed readback is not itself proof
of a dead cursor queue. Input latency under that load remains a runtime gate.
See `docs/HALCYON-INTERACTION.md` section 14.

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
a successful graphical grant.

A failed seat is recoverable. The kernel has already cancelled the grant and
scrubbed the key queue, so Lictor shows the failure notice for 1.5 s, waits
for every key to come up, rebinds the last normal presentation and reports
RESTORED; the kernel returns the seat to normal and releases nothing. Normal
broker requests park through a failure exactly as through an episode, so the
compositor never sees an error for a display that is about to return. A
refused restore (work still in flight, a dead device) is retried every pass.
Before 2026-09-21 a failure was terminal and reachable by benign timing: a
chord held past the 5 s quiesce deadline left the display dark until both seat
processes died.

When the compositor unrefs the resource it is presenting, the broker forgets
that presentation, and a takeover that ends before a successor is bound
restores to a blank output for the compositor to repaint. A failed episode cancels its still-unredeemed
pending grant by exact target incarnation and session. Grants are held against early redemption until the RESTORED commit.
Redeemed grants then use the existing legate lifecycle.

### Trusted visual layout

`render.rs` follows the approved HTML's dark green-black palette, 48 px rail,
660 px bordered panel, left fasces, 29 px proportional heading, separate
principal/PID columns, amber-edged capability area, lifetime explanation,
outlined key field and footer actions. An axe and explicit termination wording
accompany CAP_KILL. Every requested capability retains its exact kernel name.
The PID is authenticated model data; no executable label is fabricated from the
HTML's sample request. Numeric UID and eligibility level remain visible.

`typography.rs` consumes committed alpha masks and generated metrics from
`tools/bake-lictor.py`: repository-owned IBM Plex Sans and Cornucopia, 97 glyphs
in each of eight fixed faces, 328151 bytes total. No TTF parsing, theme read,
user atlas or GPU text renderer enters the trusted path. The host example
`render-preview` uses the same rasterizer but is not an authorization channel.
Normal OS builds require no Python font tooling.

Layout measures the actual baked glyph advances before painting. It chooses a
compact spacing/type variant if the ordinary layout will not fit; long names
and notices wrap rather than being ellipsized. Invalid semantics, mask counts,
geometry or backdrop length are rejected before any destination pixel changes.
The minimum remains 800x720. Host tests cover all 127 nonempty capability
subsets across nine verdict states with maximum-length principal, level,
notice and lifetime values. This is bounded-layout evidence, separate from
real display/input qualification.

The trusted scene uses a solid neutral background, accepted by the operator
on September 24. Capture and blur are not required. The broker holds a normal
resource, not a private completed-frame snapshot; no live compositor memory is
sampled. `docs/LICTOR-BACKDROP-REVIEW.md` retains the closed, unimplemented
capture proposal and its architectural findings.

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
retains reservations instead of freeing potentially live backing. Failed takeover
and stale or expired generations cancel the episode and recover as above. Owner
death is different: a dead service leaves the seat failed until warden starts a
new one, which must re-establish hardware ownership; no ordinary client
inherits the seat. The ledger validates a request before it reserves an id, so
a refused request leaves no reservation behind.

## Performance

Normal rendering adds a bounded broker round trip. Trusted scenes use CPU
rasterization into private DMA; the backend uploads complete frames on semantic
or mask changes. The neutral background avoids unqualified capture operations.

## Prosecution

QEMU graphical regression passes empty, confer, actual DAC elevation, abdicate,
wrong-key and Escape paths, all entered with the primary Ctrl-Alt-F10 chord; `ls-graphical-sak-states`
adds real expiry and the five-failure lockout at the 800x720 minimum; and
`ls-graphical-sak-recover` holds the chord past the quiesce deadline, requires
the failure notice, the recovery with nothing conferred, a live workspace and
then a full conferral on the recovered seat. Ordinary test descendants are refused trusted-seat
operations and GPU claims at every stage. The run disables serial authorization.
Broader accelerated/backend and SMP verification remains in progress. Host tests exercise framing,
semantic validation, ownership/retirement, keymaps and rendering rejection.
Kernel tests exercise seat role gates, physical-input admission, generation,
visibility acknowledgement, held-key restoration and pending-grant cancellation;
`cons.graphical_seat_grant_and_failure` drives the grant through the seat
operation, its commit at RESTORED, its cancellation by a failure and the
recovery that releases nothing, and pins that a seat failure leaves a serial
episode open (sabotage-verified: without the ownership check exactly that
assertion fails, 1572 of 1573). `cons.graphical_seat_deadline_and_death`
expires each of the three deadlines through a test seam (they are 5 s / 90 s
of wall clock) -- a quiesce never acknowledged, a prompt left open whose queued
secret must not survive into the next episode, a restore never reported -- and
kills the CLIENT mid-episode through the real ZOMBIE chokepoint (failed,
episode closed, no dangling client, no bind to a failed seat, a replacement
binds once it is NORMAL). `cons.graphical_seat_service_death` does the same to
the SERVICE, with a positive control one variable away so the "serial cannot
take over a failed seat" refusal cannot be satisfied by a disabled serial
posture. `devsrv.seat_import_gates` drives every refusal arm of
`SYS_SEAT_IMPORT`: not the designated service, no `CAP_HW_CREATE`, not a
connection, the CONFUSED DEPUTY (a share is importable only through its
owner's connection), a dead peer, ANON, plain DMA, over the service's I-34
DMA allowance; that an identity refusal consumes nothing; and the import as a
pin -- the peer tears its whole side down and the chunk lives exactly as long
as the service holds the handle. `sys_spawn_with_perms.seat_roles` pins who
may confer the three seat roles and the ordered first-come bind.
These tests are blind to actual scanout/input timing and backend DMA isolation;
QEMU end-to-end qualification and real screenshots are separate evidence. No
Pi hardware qualification is claimed.

### September 24 verification

On main base `5ed51ff5` plus the fresh-checkout vendor repair and visual changes:
25 pure host tests pass; the QEMU HVF `ls-graphical-sak` gate passes (90 s),
`ls-graphical-sak-states` passes at 800x720 (185 s), and
`ls-graphical-sak-recover` passes (66 s). A fresh kernel build followed by
`tools/test.sh` reaches the boot banner with 1667/1667 kernel tests passing.
External Alpine and Clade fixture gates are explicitly skipped, not coverage.
The author inspected actual request, masked, denied, lockout and restored
screenshots. This is a single-agent self-review under the operator's standing
instruction, not a new independent adversarial audit. Main's queued B-1c/aux
landings still need integration verification before this branch is merged.

## Seams

[[sub-warden]] supplies boot grants, [[sub-tapestryd]] supplies normal rendering,
[[sub-corvus]] supplies policy and semantic frames. [[abi-trusted-seat]] and
[[abi-native-nonblock]] pin the native boundaries. `caps-probe --seat-*` is an
ordinary guest test client, never a source of trusted authority.

## Caveats

The current backend is QEMU virtio GPU/input with a neutral backdrop. Pi DMA,
controller ownership and display conformance require separate qualification.
The physical hypervisor and host GPU renderer remain trusted for a VM. The
author's review was single-agent self-review by operator direction. A second
review by a different agent followed on 2026-09-21, also in-session by operator
direction (memory/audit_lictor_closed_list.md); it is context-independent of
the author but it is not a separate prosecutor run.

Closed after that review, before the merge: the fence exhaustion. One monotone
32-bit sequence latched the engine dead at 2^32 commands -- weeks of uptime.
The id was doing two jobs, so it is now two sequences (`usr/lictor/src/fence.rs`,
`backend/gpu.rs`). The WIRE id is what the device echoes: legacy virgl
callbacks carry 32 bits and the device retires a legacy-fenced command when
`its id <= the signalled id`, so the only rule is monotonicity AMONG COMMANDS IN
FLIGHT -- and the sequence rewinds to 1 once it has passed 2^31 and the device
holds no chain at all (no tagged fence, no abandoned chain still to retire, no
synchronous chain in its window). The OWNER id is what a fenced command's
submitter is told and what its completion carries back: a u64 that is never
reused, so nothing that matches a completion to a request (the compositor's
readback record does, by equality) can alias across a rewind. The batched pair
samples idleness ONCE: its second id is taken as busy, because the first is
about to be in flight beside it and no flag shows that yet. Clients are
unaffected either way -- their ledgers count completions, they never compare
ids. Test builds start the wire sequence 64 short of the rewind point, so
every gate that reaches the desktop has crossed a real rewind on the real
device; `ls-graphical-sak` asserts the witness line. A device that is never
once idle across the last 2^31 ids still fails closed.

Open after that review:

- `test-mode` is still a default cargo feature of `lictor` and `tapestryd`, so
  every image carries the levers (the #880 strip-for-production class). The
  production compile is now VERIFIED (2026-09-21): `cargo check --release -p
  lictor --no-default-features --features backend`, and `-p tapestryd` /
  `-p halcyond` with `--features guest`. It did not build before that: the
  broker ran `Gpu::pair_protocol_selftest` -- a `cfg(test-mode)` method -- on
  request in every build. The verb is now refused (`BadField`) without the
  feature, like the four `Test*` verbs beside it; nothing in the tree sends it
  (the backend runs the selftest itself at init).
- A display smaller than 800x720 refuses the whole seat at startup. There is no
  reduced layout.
- The main loop polls at 100 Hz whether or not anything is happening, and the
  compositor asks for the seat state every pass. The idle cost is unmeasured.
- Warden starts the compositor after the service signals READY and never reaps
  it.

## Provenance

Architecture and portability decisions are recorded in
[[dec-2026-09-18-graphical-sak-portability]]. Runtime evidence is tracked by the
integration verification record when the change lands.

HI-0 runtime checkpoint (2026-09-24): `ls-halcyon-pointer` passes on QEMU/KVM
with the 2D and VirGL/EGL backends at 1280x800, scale 100. Actual VNC cursor
planes show five shapes, edge clipping, divider selection and owner retirement;
the SAK plane is fully transparent and the cursor returns after restoration.
Raw fixture interiors remain byte-exact before and after pointer changes.
This qualifies those composed paths, not bare-metal Pi or direct scanout.
