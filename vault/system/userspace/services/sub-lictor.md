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
  - usr/lictor/src/eventq.rs
  - usr/lictor/src/backend/import.rs
  - usr/lictor/src/backend/input.rs
  - usr/lictor/src/backend/intake.rs
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
  - tools/interactive/ls-graphical-keys-overrun.exp
  - tools/interactive/ls-graphical-keys-behind.exp
  - tools/interactive/ls-graphical-sak-states.exp
  - tools/interactive/ls-graphical-sak-recover.exp
  - tools/interactive/ls-graphical-sak-dead.exp
  - tools/interactive/ls-graphical-sak-chained.exp
  - tools/interactive/ls-graphical-sak-soak.exp
  - tools/interactive/ls-graphical-sak-start.exp
  - tools/interactive/ls-graphical-sak-refused.exp
  - tools/interactive/ls-graphical-sak-unread.exp
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
updated: 2026-11-10
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
the serve thread, the only one that calls the seat endpoint, can ACK into EXCLUSIVE.

## October 1 production-debt recheck

The stable Main/Aux tree retains the open defaults/geometry/lifecycle items
listed below: test features enabled by default, 800x720 minimum, 10 ms broker
poll interval with no measured idle cost, and no supervision/reap after
successful compositor readiness. Tapestry's idle throttle and GPU resource
reaping do not close the latter two findings. Pi 400/500 qualification remains
open. The solid backdrop is the operator-selected presentation; blur is not an
outstanding implementation requirement.


## Walk answers (2026-11-03)

The broker's Twalk answers a miss on its first name with `Rlerror(ENOENT)` and on a later
name with a short Rwalk, neither binding newfid, where it used to treat every miss as a bad
request and drop the connection. A dead ring is a miss. The kernel now reports what a
server says about a walk (ERRORS.md ER-2), and its only client, `proxy/gpu.rs`, treats
every failure alike. `..` from the root is the root.

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
context's resource, retire or drop of a host ring) runs only once every chain that
can reach its object has really retired (GPU-DESIGN 4.5.15, vote 49). Each pending
request asks `Device::retirement_hold`. For a resource the answer reads every stream
on a context holding it (`Objects::holds`: attached, or a host blob minted under
that context) and every fenced transfer naming it. For a context, or a detach from
one, it reads that context's chains, transfers included. Each slot's `fslot_reach`
records what its chain can reach and survives abandonment, because an abandoned
chain still can. Device-wide synchronous work holds every retirement, and a dead
engine refuses every one. The cursor queue holds none: its chains touch only a
cursor plane. The hold has three values (`gpu_api::Hold`): `Clear` runs, `Parks`
waits, `Refused` is admitted at once. A park must end: an episode ends, in-flight
chains retire or are abandoned, and a stalled engine revives or dies (below). A
poisoned chain may never end, so what only it holds is `Refused`, and any retirement
parked `PARK_BOUND_MS` (30 s, asserted equal to the fenced abandonment bound) is
admitted whatever the cause. Either way `Device::execute`
refuses it before any ledger change, says so (at the 1st, 2nd, 4th, ... refusal, with
the cause: a non-quiescent engine, or a poisoned chain that can reach the object),
and the compositor degrades instead of blocking on a reply that never comes.
Another client's poison reaches nothing of this one's, so it refuses and parks nothing
here (`tools/warp/warp-poison-retire.exp`). Scoping relies on the host resolving a
stream's ids through its context's table (I-45's trusted half).
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
still outstanding after 30 s latches the engine dead, permanently, and so does one
still outstanding `TAKEOVER_BOUND_MS` (33 s) after a chord whose takeover waits on it
(vote 80, below), whatever the stall's own age; Lictor says which bound fired. The stall
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
compositor by the count alone. `Request::TestStallShot` (opcode 70) gives one
more shot outside that budget, with no arming: tapestryd's test-mode
`warp-stall-create` and `warp-stall-destroy` verbs aim it at a client's own next
device-context create or its own ctx's destroy, and bound it to a few per boot. The shot rides exactly the next synchronous
submission (`Controlq::stall_shot_bound`): one refused, unfenced, or ended
before its chain retired spends it and says `stall shot LOST`, rather than
leaving it armed for whoever sends next. So does a command refused at staging
(`Gpu::req`, while the engine takes no commands). The shot still rides the next
synchronous command whoever sends it, so a seat takeover landing between the
compositor's aim and its command would take it. That needs test-mode, the
bootarg and the operator's chord, and vote 30 recovers the seat.
`CtxAttachResource` links the attachment in the ledger before the device
answers, and a failure that left nothing (`left_nothing`) unlinks it, so the
resource can still retire. `CtxDestroy` retires the context in the ledger before the
device answers, and any failure but a dead engine un-retires it
(`Objects::unretire`): a destroy a stall refused before staging, or one a stall
met, can be sent again. The id stays named either way, and a repeat of a destroy
that ran is harmless, since QEMU's CTX_DESTROY has no error path and
virglrenderer only drops the handle from its table (QEMU v10.0
`virgl_cmd_context_destroy`, virglrenderer 1.1.0 `virgl_context_remove`). tapestryd aims the arming's shots at the
compositor context's create, at one blit-convention attempt, and at a
composition health step once GPU composition runs.
Only the designated compositor reaches the broker, and only the booting
operator sets the token. `tools/interactive/ls-gpu-dead.exp` is the gate for
the retirement shots, and `tools/warp-host.sh composed-stall` (the GL host) for
the stall shots.

### Episode

Physical Ctrl-Alt-F10 (with Ctrl-Alt-Delete retained for compatibility)
starts a kernel generation (the kernel scans the chord; [[abi-trusted-seat]]). Normal hardware requests
park while the service waits out the synchronous work and the cursor queue, excludes every advertised
scanout and selects private, non-shareable trusted backing. Held keys and buttons
must be released before Corvus can receive input, and the kernel refuses RESTORED
while its own table shows a key down. Both read the device's reports, and a device
with no room for a report drops it whole and tells nobody, so a release can go
missing behind an input thread that stopped draining. The device cannot be asked
what is down. So the eventq is as deep as QEMU offers and one page holds (`eventq.rs`,
64 against 16 before), and when a drain finds less than a report's worth free, every
key this side holds is released through the normal path, to the kernel and the
compositor alike. That count is taken after the recycle is published, against what
was offered before it: the device moves it, it never goes back, and a drop at any
moment before the read leaves it at least that close, so it can overstate a drop and
never miss one. A key still down loses one press, and reads as released to both gates,
so after an overrun the chord can be physically down when ACK goes out; Delete and F10
have no rune, so nothing reaches Corvus from them. A release that
never came would park the seat on every chord until that key was pressed again. This
recovers the seat, not the keystrokes: a report the device dropped is gone
(`ls-graphical-keys-overrun`, and `ls-graphical-keys-behind` for a real client's
queue). The same release runs where an entry found the input queue full (see
Concurrency). Whatever the devices had placed as the seat returns was pressed at a trusted
screen, and the input thread may not have moved it yet: until a pass of the input
thread begun after the return has been taken (`intake::passes`, `Seat::tail_until`), what
it hands over reaches the kernel and `held`, not the compositor, and the seat says how
much it kept back (`ls-graphical-sak-dead` leg H, whose lever stops the input thread
through the held restore). A paint in phase 2 retries after a
stall and fails the episode on an answer, and in neither case does VISIBLE go out for
it (legs F and G). The takeover line reports how many fenced chains stood on the device
when the takeover reached it and how long it took. Operation 65 admits only
standard normal pointer shapes, scale and position from the authenticated
compositor. Private cursor DMA never enters the normal object ledger. Before
trusted scanout every cursor plane receives a transparent image followed by a
hide, with each queue retirement checked. The transparent replacement also
clears display listeners that ignore visibility updates. A cursor chain that
misses its deadline stalls the lane, exactly as the controlq stalls:
- the queue is not idle, so cursor commands are refused and quiescence waits;
- the completion pump retires the chain late (`cursor_settle`), with the same
  validation an on-time chain gets;
- a malformed entry fails the queue, and a chain outstanding for 30 s abandons it
  until that chain retires.
A controlq step on the plane that stalled (the image upload, the plane's
one-time create and attach) is not the lane's failure, since the controlq's own
stall holds quiescence. A stalled create or attach is taken as done, because
repeating it would collide with what it made. A failed or abandoned cursor queue
prevents trusted acknowledgement, and a failed one retains its backing until GPU
reset. The two queues share the device loop, so a host-synchronous step (an
egl-headless flush, an upload into a busy resource) can delay a cursor chain, which
stalls at its ordinary bound rather than hold the serve loop. A readback no longer
can: it is parked until its work is done (vote 81, below).
Input latency under that load remains a runtime gate.
See `docs/HALCYON-INTERACTION.md` section 14.

Corvus sends bounded semantic frames containing identity, exact capabilities,
term and verdict. Lictor uses baked fonts and colors. Its panels say "Press
Enter to return": Corvus closes them only on Enter or Esc
([[dec-2026-11-05-panels-enter-or-esc]]). Corvus receives key bytes
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
`Device::restore` needs only synchronous quiescence
(`Gpu::synchronous_clear`), so neither a cursor queue poisoned on a live engine
nor a client's poisoned chain costs the display. Takeover itself asks that and an
idle cursor queue (`Gpu::takeover_clear`, vote 50): a fenced chain, in flight or
poisoned, cannot reach the trusted image, whose id `Objects::valid_id` refuses on
every named route and which is never attached to a context, so a client's poison
does not deny the SAK. Its backlog can: the paint is fenced, so it retires behind
what the host has queued. A takeover that meets a stall starts over when the engine
revives (`Gpu::stall_pending`), under the kernel's five-second deadline, and says so; a
device's answer fails it at once. While the engine is alive and the device holds the
takeover up (`Gpu::takeover_waiting`: the cursor lane stalled behind its chain, or, with
the lane idle, the control queue stalled behind a chain or with a command not yet
retired), the takeover renews that deadline at once when first seen and then at most
once a second (`gpu_api::renew_due`; `SEAT_RENEW`, up to 35 s from the chord: vote 71), so a backlog delays the SAK rather than failing
it. One chain that runs for seconds retires nothing and is renewed all the same; one
that never retires kills the engine at `FENCE_ABANDON_MS`, inside the 35 s. A cursor
chain waits on the host loop whatever holds it, so the lane's stall renews too
(vote 79); its abandonment at the same bound ends the wait and the kernel's deadline
fails the chord, and the lane comes back on the late retirement. An abandoned or failed
lane holds the takeover past anything the control queue does, so it renews nothing, and
Lictor says once per chord that the lane holds it. The two waits can run in series (the
paint and the activation each wait for an idle lane), each bounded 30 s from its own
stall, so the second could still stand at the kernel's limit with the engine alive and
nothing on the glass. Every wait is therefore bounded from the chord too (vote 80,
[[dec-2026-11-09-takeover-bound-from-chord]]): the seat stamps a generation's first
sight in QUIESCING, which is the reply to the key that made the chord, and hands that
instant to the device only across the phase-1 reap, the one pump after a STATUS that read
the takeover still waiting (`Gpu::bound_takeover` set before it and cleared after it, round
5; round 4 had cleared it at the end of each step). Every other pump runs without it: the
kernel fails an expired chord lazily, at a call, and a later pump could otherwise kill a
stall only the restore waits on (round 4 F1). At `TAKEOVER_BOUND_MS` from it a control stall
still standing kills the engine (`settle_stall`) and a cursor stall abandons the lane
(`cursor_settle`), both through `gpu_api::stall_verdict`, whose host test reads
`SEAT_QUIESCE_LIMIT_NS` out of `seat.h` and asserts the bound plus one renewal interval
falls inside it (and that a renewal a pass late still lands inside `SEAT_QUIESCE_NS`).
No renewal follows either end, so the kernel's limit fails the chord by 35 s unless an
abandoned chain retires first and the takeover goes through: every chord ends in the
trusted screen, a dead engine, or a logged failure. The cost falls on a takeover still
waiting at 33 s: a second wait in series dies younger than 30 s, and a takeover's own
command stalling after 33 s dies at the next seat step's reap if the chord is still live,
otherwise the restore waits on it. The phase term of `waiting_chord` is redundant (the
reap it bounds is phase 1's); the activation term keeps a stall in phase 1 after
activation clear, which no witness constructs (it needs the chord held past 33 s after
the trusted screen is up). Witnesses:
`ls-graphical-sak-dead` leg 1 (a stall withheld 8 s, no retirement in between), leg C
(a cursor chain held 8 s: the takeover completes), leg X (one held past the abandonment:
no renewal follows it, Lictor names the lane, the chord fails, then the lane revives),
`ls-graphical-sak-chained` (its own lever, `Request::TestArmChained`: a control stall
held 8 s and then a cursor stall held past every bound, which the takeover's bound
abandons before the kernel closes the seat, after which, at 34 s, a control stall is
withheld and the seat failed behind it, and that stall must survive to its release and
the restore; then a cursor stall held 8 s and a control stall withheld for good, which
the bound kills, last because the engine stays dead) and
`ls-graphical-sak-backlog` (a client's deep queue, on a virgl device). Every lever raises
its hold within milliseconds of the chord, so renewal at first sight rests on
`gpu_api`'s host test alone, which fails on the old rule of arming first. A frame paint that meets a stall stays owed and the
next pass paints it again. Once serving, no synchronous command waits past
`SUBMIT_DEADLINE_MS`, a client's readback ahead of it or not (operator vote 72): one
transfer cannot stop this loop for as long as its client queued; the command stalls
the engine, which revives when the chain retires, and what a client sent meanwhile is
refused. A serving `cursor_command` stalls the same way. A chain past
`FENCE_ABANDON_MS` is abandoned rather than poisoned: a well-formed late retirement
frees the lane (`cursor_queue::Queue::abandon`). A restore refused by a stalled engine
is retried every pass until the engine revives or dies. Restoration stays owed until the
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

A refused INPUT loses its batch. A refused chord moves no generation, so nothing else
tells the compositor a release it was owed is gone: `Seat::input` clears every device's
batch and pushes one reset (SYN_DROPPED), unless input is withheld. The reset carries no
key content. The witness is `tools/interactive/ls-graphical-sak-refused.exp`.

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

Lictor runs two threads. The input thread (`backend/intake.rs`, operator vote 72,
2026-11-05) owns the virtio-input devices from the end of probe. Every 4 ms it drains
each eventq into one queue of 4096 entries behind a futex `Mutex`, so a serve loop held
in a synchronous GPU wait (up to `SUBMIT_DEADLINE_MS` behind a stall) leaves no ring
undrained. It never calls the seat endpoint and never prints: a console write parks in
the exclusive phase, and the thread that drains the rings must not park there. The
ring's own notes (the test-mode low-water line, a malformed used element) travel in the
queue, and the serve thread prints them. The serve thread swaps the whole queue out
each pass and keeps every seat decision: what a key means, the kernel INPUT call, the
compositor batches, the held-key table. An eventq overrun travels as an entry at its
place in the order. An entry that finds the queue full is dropped and marks its device;
the mark is set only while the queue is full and is taken with the entries, so the loss
follows everything taken with it, and the serve thread releases that device's held keys
there (`lictor: input queue overran`). A lost diagnostic note marks nothing. InputInfo
answers from values read at probe, since the device belongs to the input thread.
Nothing joins the thread: `t_exit_group` ends it with the process, and no input DMA is
released before then. Under the armed test latch, F8 stops the input thread at the pass
that read it, before that batch is queued, and the serve loop resumes it after its own
five-second stop, so `ls-graphical-keys-overrun` still overruns the ring.

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
death is different: nothing restarts a dead service (warden leaves a persistent
service unsupervised once it is READY), so the kernel fails the seat and then
returns it to normal at a new generation, releasing nothing. A live service's
recovery moves it too, and every reply is taken whole through `adopt`, so `SeatState`
never serves a new phase with the old generation. On a boot with
`thylacine.serial-sak=1` the serial BREAK works again, the display stays dark for
the boot, and no ordinary client inherits the seat. Either
way a seat can reach normal after a SAK's note and before Corvus asks, so Corvus
reads the episode's medium from QUERY's `code` and not from the phase
([[dec-2026-11-02-seat-episode-medium]]); `endpoint.rs` reads the reply and its
host tests pin that the phase plays no part. The ledger validates a request before it reserves an id, so
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
then a full conferral on the recovered seat. `ls-graphical-sak-soak` cycles the
seat's return with no lever: each cycle ends one episode the clean way and one
past the quiesce deadline, and after each the link must rejoin, typing must reach
the shell and the next chord must take (8 cycles by default, `LS_SOAK_CYCLES` for
a hunt). `ls-graphical-sak-dead` fires the
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
ordinary guest test client, never a source of trusted authority. Its DAC witness
is two legs, because a create alone cannot tell the gates apart: elevation
overrides rwx and is not a session end (IDENTITY-DESIGN 9.7 M7), so the probe's
create on system's mount is refused at every stage, and only its open of
`/thylacine-version` for write changes with the grant.

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

**Parked readbacks (operator vote 81, GPU-DESIGN 4.5.13).** On thyla-pi, QEMU
decoding a client's readback of a busy BO sat in V3D's wait-BO ioctl for the
BO's whole queue holding QEMU's big lock, so no vCPU completed an emulated MMIO
write, and this loop's queue notify froze before any timed wait began. The
fenced lane therefore parks a `TRANSFER_FROM_HOST_3D` while its context has a
chain the device may still run (in flight, or abandoned and not yet retired),
and parks every later chain of that context behind it (`gpu_api::must_park`).
The completion pump publishes a parked readback once its context is clear, with
the chains behind it, up to that context's next readback (`gpu_api::unpark_plan`).
- **Ids.** The owner id is minted at submission; the wire id is taken at
  publication (`Controlq::publish_staged`), so the device sees wire ids in
  publication order.
- **What the lane counts.** A parked chain holds its slot (`fslot_parked`) and
  counts for its context (`ctx_fences_in_flight`), and for the compositor's pump
  (`fenced_outstanding`). It is not on the device: `fenced_in_flight`, which the
  seat's takeover lines count, `device_idle` and the abandonment clock leave it
  out. The test build's stall census (`fenced_held`, `Request::FencedHeld`)
  returns parked chains as well, flagged (`gpu_api::HeldRow`; the wire carries
  an 8-tuple), so a slot a parked chain holds is named, not missing.
- **Retirements.** A retirement that a parked chain can reach parks
  (`retirement_hold` counts parked reaches as in flight), so no detach or unref
  runs under a chain that has yet to publish.
- **Stall and death.** Nothing publishes while the engine refuses. A dead
  engine releases every parked chain as refused (`ok = false`, not abandoned):
  the device never saw it.
- **Abandoned prefix.** When a context has an abandoned chain not yet retired
  (`ctx_poisoned`), the pump releases every parked chain of that context as
  refused (`gpu_api::Unpark::Release`). The abandoned chain may never retire and
  a parked chain has no abandonment clock, so a chain left parked would keep
  `fenced_outstanding` above zero, and tapestryd's poll at 1 ms, until the
  context died. For the compositor's readback the release also frees the one
  reserved slot for every other surface. A released tag carries `released`, so
  the seam can tell it from a chain the device answered with an error.
- **Released context.** `gpu_api::release_tag` builds the released tag and
  records a client chain's context in `Controlq::released`
  (`gpu_api::Released`, one entry per device context). From then on
  `must_park` parks every chain of that context and `unpark_plan` releases it,
  until a `ctx_destroy` of the device context the released chain rode completes
  and forgets it (a destroy whose outcome is unknown keeps the record). The
  entry is held by pub, because a seam context's chains ride two device
  contexts (GL and venus), and tapestryd reissues no pub while an entry may
  still name it. tapestryd latches the stream only when its next pump takes the
  released tag, and Lictor runs the pump's release in the reap that trails a
  request, so tapestryd can admit one more chain from its older view.
  Published, that chain would run after a readback that never ran and its
  completion would count past it. A compositor readback's release records
  nothing, the same as tapestryd.
- **The harness hold.** A chain whose retirement the test hold swallowed
  (`held_retires`) has finished on the device, so nothing waits for it.

Each tag carries `parked_ms` and `exec_ms` (publication to retirement), and a
test build says a readback that parked or ran 20 ms or more when its park or its
run at least doubles the last one said (`Controlq::rb_said`), so the say is
bounded per boot. The C-6 gate's
DEADLINE arm judges both.

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
