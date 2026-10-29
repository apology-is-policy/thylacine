---
id: sub-lictor
type: sub
title: "Lictor - the trusted graphical seat and normal hardware broker"
parent: moc-userspace
code:
  - usr/lictor/src/quiescence.rs
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
  - tools/interactive/ls-graphical-sak-dead.exp
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
updated: 2026-10-29
---
## Clipboard cancellation progress boundary

Trusted input now additionally requires `quiescence::Gate`'s exact-generation
aggregate acknowledgement from the designated compositor's fresh kernel PID
and stripes. Typed opcode 66 is SeatQuiesced; 65 remains Cursor. SeatState and
SeatQuiesced progress even while ordinary GPU requests are parked. A wrong
phase/generation cannot authorize the episode; hardware retirement and released
physical keys must also pass. Missing cancellation reaches the existing bounded
failure/restoration path. No clipboard payload, new kernel role or syscall enters
Lictor. Native healthy/stalled physical F10 probes and graphical regressions
are pinned in HALCYON-INTERACTION-STATUS; no Pi/minimum-mode qualification is
claimed. The test-mode parked-RPC marker is emitted only in QUIESCING, before
this sole hardware-owner thread can ACK into EXCLUSIVE.

## October 1 production-debt recheck

The stable Main/Aux tree retains the open defaults/geometry/lifecycle items
listed below: test features enabled by default, 800x720 minimum, 10 ms broker
poll interval with no measured idle cost, and no supervision/reap after
successful compositor readiness. Tapestry's idle throttle and GPU resource
reaping do not close the latter two findings. Pi 400/500 qualification remains
open. The solid backdrop is the operator-selected presentation; blur is not an
outstanding implementation requirement.


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

A retirement (detach or unref of a resource, destroy of a context, detach of a
context's resource, retire or drop of a host ring) runs only once every
outstanding chain has really retired; until then the loop leaves it parked
(`gpu_api::admit`, asked of each pending request). A park must end: an episode
ends, in-flight chains retire, and a stalled engine revives or dies (below), but
some quiescence never comes back. A dead engine and a cursor queue poisoned
until device reset (`Gpu::quiescence_unreachable`) admit a retirement at once; a chain the device
may never retire (a poisoned fenced slot) is bounded by the clock instead, so a
retirement parked `PARK_BOUND_MS` (30 s, asserted equal to the fenced
abandonment bound) is admitted whatever the cause. Either way `Device::execute`
refuses it before any ledger change, says so (at the 1st, 2nd, 4th, ... refusal),
and the compositor degrades instead of blocking on a reply that never comes.
The pins themselves stay because nothing frees one without a device
completion: a refused detach keeps its backing, and `reap` releases an unref's
pin only once the device journal reports it, which a dead engine never does.
`execute`'s refusal keeps the ledger consistent, never leaving a `retiring`
entry that `reap` could not finish. The bound is phase 0's only: an episode's
own deadlines end its park, and ordinary admission stays closed for the whole
trusted span. Until 2026-10-29 a retirement on a dead engine parked: one
command that missed the synchronous deadline took the display away for the
rest of the boot, measured twice on thyla-pi (a boot-time blit probe, and a
runtime console transfer).

The synchronous deadline is a two-stage latch (GPU-DESIGN 4.5.15, operator
vote 29). A chain that sees 500 ms of stale wakes STALLS the engine:
- the command fails with an unknown outcome;
- its chain stays outstanding and nothing is abandoned;
- every new submission is refused (synchronous ones would reuse the
  outstanding chain's buffers; fenced ones get the retryable `Again`;
  cursor commands too). For a synchronous command the refusal comes before
  it is staged: every builder takes the request region from `Gpu::req`,
  which refuses while a synchronous chain is outstanding, because the device
  reads a chain's request only when it processes the chain and would
  otherwise run the staged bytes as the published command;
- the serve loop stays live, and a retirement parks.

The completion pump settles a stall (`settle_stall`, decided by the
host-tested `gpu_api::stall_verdict`). A late retirement, drained by the
ordinary path, revives the engine, and the parked retirement then runs. A chain
still outstanding after 30 s latches the engine dead, permanently. The stall
counter tells a caller whether its command stalled, and a stalled bind
condemns its resource, because the display may name it once the device
catches up. Every reply carries the engine state (live, stalled, dead) and the
stall count in its `Stats`, an error reply included. The client keeps the last
one (`rpc_client::Client::stats`), and the GPU proxy adopts it on every reply,
so a caller reads a failed step against the state it left. The host-tested
`gpu_api::failure_cause` decides what such a failure says: a stall begun inside
the span, or one still standing, leaves some outcome unknown; a dead engine
outranks every stall; otherwise the device answered. tapestryd's composition
health tick takes no verdict from a stall that way ([[sub-tapestryd]]).
Bring-up (`Gpu::probe`, `Screen::new`, before
`Gpu::begin_serving`) waits out the full 30 s instead: there is no loop to
keep live.

The GPU command machinery remains in the trusted service. Its synchronous and
asynchronous commands share one monotone fence sequence. Synchronous responses
must return the requested fence; queue acceptance alone is insufficient for
trusted takeover. Legacy Virgl callbacks bound the sequence to 32 bits; exhaustion
fails closed rather than wrapping or using an incompatible high-bit namespace.

The synchronous deadline reports its own margin. Each new slowest synchronous
command past 50 ms is said. A stall is said when it begins, and so is its end:
a revival, with how long after the deadline the serve loop saw the chain retire
(an upper bound, since the loop looks once a pass), or a death. A chain the
device retires after a death is also said once. Together these tell a late
device from one that never completes.

A test-mode lever reproduces both ends of a stall on demand.
`thylacine.test-latch-dead` on the kernel command line
(`THYLACINE_TEST_LATCH_DEAD=1` to `tools/run-vm.sh`) makes tapestryd arm the
broker with `TestArmLatch` once it serves, and its global ctl says
`test-latch-dead armed` once the broker has taken it. `Device` then marks each
resource created at 97 by 89. At a retirement of one, it stalls the engine
through a harmless display query whose wait reports its deadline passed,
exactly the state a slow command leaves. The lever has two shots, and each one
forgets the marks made before it:
- on the first shot the chain retires, so the engine revives and the retirement
  completes, said with its pins released;
- on the second shot the chain's retirement is withheld, so the engine dies at
  30 s and the retirement is refused.
The same arming gives the compositor three stall shots to aim
(`Request::TestStallNext`, refused once spent). Each makes the next synchronous
command retire, then report its deadline missed (`Gpu::test_stall_next`),
said as `INJECTED stall shot of test-latch-dead`, a token distinct from the
retirement shots' so a scenario waiting on one cannot match the other. The pump
at the end of that same request (`Device::execute`'s trailing reap) revives the
engine, so the reply already reads `Engine::Live` with the stall count moved: a
stall of known length, with no race against the device, carried to the
compositor by the count alone. The shot rides exactly the next synchronous
submission (`Controlq::stall_shot_bound`): one refused, unfenced, or ended
before its chain retired spends it and says `stall shot LOST`, rather than
leaving it armed for whoever sends next. So does a command refused at staging
(`Gpu::req`, while the engine takes no commands). The shot still rides the next
synchronous command whoever sends it, so a seat takeover landing between the
compositor's aim and its command would take it. That needs test-mode, the
bootarg and the operator's chord, and vote 30 recovers the seat.
`CtxAttachResource` links the attachment in the ledger before the device
answers, and a failure that left nothing (`left_nothing`) unlinks it, so the
resource can still retire. tapestryd aims them at the
compositor context's create, at one blit-convention attempt, and at a
composition health step once GPU composition runs.
Only the designated compositor reaches the broker, and only the booting
operator sets the token. `tools/interactive/ls-gpu-dead.exp` is the gate for
the retirement shots, and `tools/warp-host.sh composed-stall` (the GL host) for
the stall shots.

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
clears display listeners that ignore visibility updates. A cursor chain that
misses its deadline stalls the lane, exactly as the controlq stalls:
- the queue is not idle, so cursor commands are refused and quiescence waits;
- the completion pump retires the chain late (`cursor_settle`), with the same
  validation an on-time chain gets;
- a malformed entry, or a chain outstanding for 30 s, fails the queue.
A controlq step on the plane that stalled (the image upload, the plane's
one-time create and attach) is not the lane's failure, since the controlq's own
stall holds quiescence. A stalled create or attach is taken as done, because
repeating it would collide with what it made. A failed cursor queue retains its
backing until GPU reset and prevents trusted acknowledgement.
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
compositor never sees an error for a display that is about to return.
Restoration is owed from the moment a takeover begins (`Seat.displaced`, set
before `exclude_all_outputs`), not only once it completes: a takeover that
fails part-way may already have disabled outputs or bound the trusted image.
`Device::restore` needs only the control queue's quiescence
(`Gpu::controlq_retired`), so a cursor queue poisoned on a live engine does not
cost the display. A restore refused by a stalled engine is retried every
pass until the engine revives or dies. Restoration stays owed until the
kernel acknowledges RESTORED: when the kernel refuses it (a grantee that
exited before the commit), the seat fails and rebinds again before it
returns. `Seat.active` names the generation whose trusted scanout is up, so
state an episode left behind can never stand in for a later takeover's
exclusion and bind.

A dead engine can restore nothing, so the seat goes back without its display
(TRUSTED-PATH section 7, operator vote 30):
- an episode that reached restoration fails first, so the grant is cancelled;
- RESTORED from FAILED then returns the seat to normal, so the serial SAK works
  again and the compositor's requests are refused at once instead of parking;
- if the takeover had begun, a frozen trusted frame may stay on the glass, so
  `Seat.withheld` keeps every later keyboard and pointer event from the
  compositor for the rest of the boot. The first such event says
  `input withheld from the compositor`.

Until 2026-10-29 the restore was retried forever: every compositor request
parked for good and the kernel refused the serial BREAK into the failed
episode.
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
a refused request leaves no reservation behind. A dead engine refuses every
retirement (its pins stay) and fails every command that would reach the ring;
queries the broker answers from its own state still answer, and seat traffic
still flows. An episode it interrupts returns the seat without its display
and withholds input for the rest of the boot (see the recovery above).

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
then a full conferral on the recovered seat. `ls-graphical-sak-dead` fires the
`test-latch-dead` lever's seat half, one shot per trusted takeover
(`Device::test_latch_seat_shot`, `SeatShot`). The first stalls the engine between disabling the outputs and binding the
trusted image, and the device retires it, so the takeover fails part-way and
lictor must say `normal scanout restored` before the seat recovers, and typing
must reach the shell. The second stalls the cursor queue inside exclusion and
holds its retirement until the seat restores, so the restore must go through
on the control queue alone (`Gpu::test_cursor_stall_next`). The third has the
kernel's RESTORED refused after a completed restore: lictor must rebind again
before the seat returns, and the next takeover must exclude and bind afresh.
The fourth stalls the engine under the trusted scanout with the
retirement withheld, so the engine dies at 30 s. The seat must recover with
its display lost, and a key typed afterwards must draw lictor's
`input withheld` line, while the shell stays silent. Ordinary test descendants are refused trusted-seat
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

During a stall every request other than a retirement fails at once, so the
present path skips frames. A retirement parks, and the compositor blocks on its
reply for the stall, up to the 30 s bound. A stall that meets the synchronous
health copy of a surface import costs that generation its GPU path, because the
import is not retried (OPEN-BUGS 2026-10-29T10:39Z). A stall ends by itself
within 30 s; a dead engine does not, and a seat episode on it returns without
its display (above).

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
