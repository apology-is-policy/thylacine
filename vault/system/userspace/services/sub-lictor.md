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
  - usr/lictor/src/type2.bin
  - usr/lictor/examples/render-preview.rs
  - tools/bake-lictor.py
  - tools/interactive/ls-graphical-sak.exp
  - tools/interactive/ls-graphical-sak-states.exp
  - tools/interactive/ls-graphical-sak-recover.exp
  - tools/interactive/ls-graphical-sak-dead.exp
  - tools/interactive/ls-graphical-sak-soak.exp
  - tools/interactive/ls-graphical-sak-start.exp
  - tools/interactive/ls-graphical-sak-refused.exp
  - tools/interactive/ls-graphical-sak-unread.exp
  - tools/interactive/ls-graphical-sak-scale.exp
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
updated: 2026-11-12
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


## Walk answers (2026-11-03)

The broker's Twalk answers a miss on its first name with `Rlerror(ENOENT)` and on a later
name with a short Rwalk, neither binding newfid, where it used to treat every miss as a bad
request and drop the connection. A dead ring is a miss. The kernel now reports what a
server says about a walk (ERRORS.md ER-2), and its only client, `proxy/gpu.rs`, treats
every failure alike. `..` from the root is the root.

## Display events, and a trusted screen for any host size (2026-11-11)

Written by aux while this dossier's keeper was away; `docs/handoffs/050` is the
note left for her.

**The device's events are taken.** virtio-gpu sets bit 0 of `events_read`
(device configuration, offset 0) and raises its configuration interrupt when
the host's preferred size or its EDID changes: a window resized, or moved to a
display of another scale. `Controlq::take_events` reads the register. If it is
not zero it writes the same bits to `events_clear` (offset 4), and if the
display bit was among them it adds one to `display_events`, a wrapping `u32`.
`service_completions` calls it when a configuration change may have happened.
On the interrupt line that is when the ISR byte, which it reads to drop the
line, has bit 1. On MSI-X the configuration interrupt has the queues' one
vector (`PciIrq::for_virtio`), nothing tells the two apart, and the register is
read once for each interrupt ticket. At start, pending events are taken and
the count is set to zero just before the first `GET_DISPLAY_INFO`: that
question asks for the state those events announced.

No event is lost. A ticket's vector is masked from its delivery until
`complete`, so the device pends a message raised in between and delivers it at
the unmask as a new ticket; on the line, `complete` finds it asserted and
replays the ticket. An event raised between the read and the clear is cleared
without being counted. It is covered by the count that call makes, because
whoever follows the count asks the device after seeing it.

**The count rides every reply.** `Stats.display_events` sits between
`condemned_lost` and `engine`. `Stats` is decoded in one place, the client in
`rpc_client.rs` that every user of the broker links, so the layout changed for
all of them at once.
The proxy's `Gpu::display_events` reads the count from the last reply its
client had, of any kind. It is not one of the proxy's own fields, because a
`query` does not refresh those and the count would then miss a reply.
[[sub-tapestryd]] asks the broker something on every pass, so it learns of an
event within a pass and needs no wake of its own. What it does then is in its
section "The display follows its host".

**An EDID that is not answered.** `query_edid` answered `Ok(None)` for every
way GET_EDID could fail. It now tells two apart. The device answered with
something other than an EDID, or with a block that is not one: `Ok(None)`,
there is no EDID, and a caller's scale is 100. Or the command was not
answered, because the engine takes no commands or its deadline passed: an
error. It reads which from `submit_and_wait` itself, which gives the device's
response type or nothing. Lictor's own start reads an error as no EDID, as
before. The caller that needs the difference is the compositor when it follows
a display event: a stall at that step must not set the scale to 100
([[sub-tapestryd]]; audit round 2 of that section).

**What the device scans, as the proxy knows it.** A scanout command whose
deadline passes may still be executed, and the broker parks its resource for
that reason (`condemn_if_stalled`). The proxy keeps the other half of the same
fact for its client: `Gpu::scanout_unsettled`. Each of the four scanout
commands (`set_scanout`, `set_scanout_blob`, `set_scanout_blob_then_flush`,
`set_scanout_blob_probe`) notes the stall count of the last reply the client
had, of any kind, before it calls. Accepted, it clears the flag: the device
takes its commands in order, so an accepted scanout command is the last word.
Not accepted, with the stall count in its reply moved, it sets the flag. Not
accepted otherwise, it leaves the flag as it was: the device answered with a
refusal, or the command was refused before it was sent, or it was sent and the
engine died under it (a corrupt ring, a fence that does not match, the 30 s
bound, an interrupt that failed), which moves no count and from which no
engine returns, so nothing is asked of the flag again. For the probe, accepted
means the device answered OK. A deadline that passed between the two replies
for a command that was not this client's reads as this command's own. No
other client sends the device work (the seat coordinator's client speaks only
of the seat), so those are the broker's own: the trusted screen's commands and
the test levers. That errs to "not known", and costs the compositor a bind and
what follows one: composed, a repaint, a whole-buffer push and a CONFIGURE to
every client; direct, a CONFIGURE and the bind of the client's present. A
client whose
connection to the broker failed adopts no reply, and its flag stays as it was;
it has no broker to ask anything of. The flag was added after audit round 3 of
the display section, where a compositor-side flag set by hand at each bind had
missed one ([[sub-tapestryd]], "What the device is known to scan").

The broker could instead settle the scanout itself when a stall ends, by
binding again what it last accepted; a client would then need to know only
that a stall happened, to push its content again. That is a change to the
stall design (`docs/GPU-DESIGN.md` 4.5.15) and is not made.

**The trusted screen's size.** `render::fits` is the one test of a size the
trusted scene can be drawn at: 800x720 at least and 4096x2160 pixels at most.
`render()` asks it, and so does `render::geometry_for` (the next section; it
was `Screen::size_for`), which gives the host's size if it fits, else that size
over or times a whole number up to 4, the first that fits ([[sub-libhalcyon]]'s
`display::fit_mode`), else 1024x768; a side is also at most 8192
(`MAX_DISPLAY_DIM`). So Lictor starts for any host: 640x400
gives a screen of 1280x800, 5120x2880 one of 2560x1440. Nothing in the device
scales a screen that is not the host's size: QEMU makes the console's surface
the size of the scanout, and whatever shows it (a window, a VNC viewer, the
Mac app) decides how. The factor is whole so that a frontend scaling by whole
pixels shows the screen sharp, and it is the first that fits, chosen at
Lictor's start and again when its display changes. Before this a host
outside the limits failed `Screen::new`, and with it the probe of the driver
that is also the keyboard and the pointer. The screen says so when its size is
not the host's or its scale is not one:
`lictor: trusted screen WxH at Kx for a display of WxH`.
`tools/interactive/ls-graphical-sak-scale.exp` reads that line, word for word.

**What it did not do, and now does.** When this section was written the
trusted screen was made once and kept the size of the boot whatever the host's
window did afterwards (`docs/handoffs/050`). It follows its display since
2026-11-12: "The trusted screen follows its display", below.

**Tests.** Host: `render::tests::the_sizes_it_fits_are_its_layouts_floor_to_its_rasters_pixels`
(each limit at and past its edge, a pixel count that does not wrap, `render()`
refusing a raster of exactly an unfit size), and the `Stats` record's layout
with the count in it. Guest: `tools/interactive/ls-display-follow.exp`
([[sub-tapestryd]]) takes events through this path. The branch for the
interrupt line is in no gate.

## The trusted screen at its display's scale (2026-11-12)

Written by aux while this dossier's keeper was away; `docs/handoffs/051` is the
note left for her. The decisions are in `docs/GRAPHICAL-SAK-OWNERSHIP.md`, "The
trusted screen's scale".

**One layout, drawn at one or at two.** `render.rs` lays the screen out in
layout pixels: the 48 px rail, the 660 px panel and every position in `draw`
are in them, and nothing in the layout knows the scale. `Canvas` holds the
raster in device pixels and `k`, the device pixels to a layout pixel. `rect`,
`outline`, `text` and `fasces` take layout pixels and multiply; only `fill`
takes device pixels. A rule or a border is `k` device pixels thick. A device
row or column left over at the far edge of a raster with an odd side is the
background's, and the rail is drawn one layout pixel wider than the screen so
that it reaches the last column.

**The type is baked at each scale.** `tools/bake-lictor.py` rasterises every
face once for each scale (`SCALES = (1, 2)`) into `type.bin` (328151 bytes, the
same bytes as before) and `type2.bin` (1310276 bytes). `type_metrics.rs`
carries each set's hash as the baker wrote it (FNV-1a) and a host test
recomputes it, so a set that is not the one its metrics were baked with fails:
a glyph changed in the file, or a set left uncommitted after a bake
(`.gitignore` exempts both from its `*.bin`). The set at two has the
box, the baseline, the bearing room and the advance of the set at one, times
two; only the outline is drawn finer. `typography::glyph(font, k, ch)` gives
the advance and the box in layout pixels and a mask `k` times as wide and as
high, and `width` measures with the advances, which are the same in both sets.
So a word wraps at the same place and a line is on the same row at either
scale, and the layout's host tests (the 127 capability subsets by nine states
at 800x720) hold for both without being run twice. Nothing is resampled at run
time: `text` reads the mask of the raster's own scale, a mask pixel for a
device pixel. The emblem's two diagonals are the only other thing finer than a
layout pixel: they step once a device row. (The layout's own test draws both as
they are at one, each pixel doubled. Its switch, `coarse` on `Canvas` and on
`draw`, is compiled for tests only; no build of the service has it.)

**Which scale.** `render::scale_at(w, h, pct)` is the nearest whole scale to
`pct` percent, so two from 150, taken only if the screen fits at it (`fits_at`:
`w / k >= 800` and `h / k >= 720`), else one. A 1280x800 screen on a display at
200% is drawn at one, because its layout would not fit at two. `pct` is the
display's scale as the session reads it, by the session's rule
([[sub-libhalcyon]]'s `scale`): the platform's `thylacine.scale` first, which
`backend/screen.rs` `declared_scale` reads from `/hw/chosen/bootargs` at
Lictor's start as the compositor does at its own, then the millimetres of the
EDID, which Lictor reads at its start (`Gpu::edid_mm`) and again each time it
follows its display, else 100. From millimetres
the percent is the session's reading, the pixels to the inch in quarters of the
reference rounded half up, so a display at 1.375 of the reference (132 to the
inch) reads as 150 and is drawn at two: on that path two begins at 137.5
percent. An EDID whose size is 16:9 or 16:10 written where the size goes
says nothing ([[sub-libhalcyon]]'s `parse_edid_mm`: six pairs, and no other
ratio), so such a display is at one unless the platform declares.
`libhalcyon::display::own_pct` carries that to the screen's own pixels: a
screen made at half its display's size is at half its display's scale, and the
frontend that shows it makes up the rest. `render::geometry_for(host, declared,
mm, side, fallback)` is the size and the scale together, a pure function;
`Screen::new` asks it and keeps both, and `paint` hands the scale to `render`.
It is compiled for the backend and for tests, so libhalcyon is a dev-dependency
of the crate as well as the backend's optional one. Nothing a client says is
asked: not the compositor's scale, not a session's setting, not a surface's.

**What it does not do.** A scale between one and two is drawn at the nearer whole
one: at 125% the screen is four fifths of the session's size, at 150% a third
larger than it. There is no scale of three. `declared_scale` says nothing when
the command line is too long or the value is not a scale; the compositor reads
the same line by the same rule and does say.

**Tests.** Host, `render::tests`: `a_scale_is_one_the_screen_still_fits_at`
(the rule and its edges), `at_two_it_is_the_same_screen_drawn_finer` (five
screens: drawn at two with the type and the emblem as coarse as at one, every
pixel of the screen at one is exactly four of its own; drawn at two in full, a
layout pixel's mean is within 48 of 255 of the screen at one where there is
type (measured 46 to 48; a set a device pixel out is about 100) and within 137
in the emblem's rectangle, which the test finds as `draw` does; and neither is
what it is at one, doubled),
`type_at_two_lands_where_type_at_one_does` (each glyph of each face alone: over
a face the ink's middle is within a twentieth of a layout pixel and its amount
within 2%), `a_pixel_left_over_at_the_far_edges_is_drawn`,
`type_past_the_rasters_edge_is_cut_off`,
`a_scale_it_does_not_have_paints_nothing`, and
`a_displays_screen_is_a_size_it_fits_at_the_scale_of_its_own_pixels`
(`geometry_for`: a display it fits, one too small, one with too many pixels, an
8K one whose half the session's rule still reads as 150, the cap on a side,
the fallback, and that every answer is one `render` takes). `typography::tests`:
`the_sets_hold_every_glyph_and_nothing_else` (the lengths, and each set's
hash) and
`a_glyph_at_two_is_the_glyph_at_one_drawn_finer` (the same advance and box;
over a face the ink's middle within three hundredths of a layout pixel).
[[sub-libhalcyon]] has `own_pct`'s. Not pixel for pixel between the two sets:
the set at one is sampled four times a pixel each way, so a thin stem's middle
is up to 0.44 of a pixel from the finer set's; the faces' means are the test.
`backend/screen.rs` is in no host build, so what it does is a guest's to show.
`tools/interactive/ls-graphical-sak-scale.exp` is the test of `declared_scale`,
of the display's size and the declaration reaching `geometry_for`, and of
`paint` handing the scale to `render`: a 2560x1600 display declared at 200, the
line Lictor says of its screen, and the rail's rule read off a dump of the
screen (device rows 94 and 95 at two; row 47 at one; the dump must hold the
whole of each rect read). It does not show the other three values
`Screen::new` hands over: the EDID's millimetres, which a declaration
outranks, the largest side, and the fallback. A wrong one of those passes it. The fallback's size is checked where it is compiled
(`const _` in `screen.rs`). The baker itself is checked by baking again and
finding nothing changed, which needs fontTools and is no gate's step.

## The trusted screen follows its display (2026-11-12)

Written by aux while this dossier's keeper was away; `docs/handoffs/052` is the
note left for her. The rules and the three decisions are in
`docs/GRAPHICAL-SAK-OWNERSHIP.md`, "The trusted screen follows its display".

**When.** `main.rs`'s pass calls `Screen::follow` after `Seat::step`, and only
when the seat's phase is 0: no trusted episode is up, beginning, or owed its
restoration. `follow` also returns at once while the screen is up (`active`).
It has its own `libhalcyon::display::Follow` ([[sub-libhalcyon]]), the one the
compositor uses, over the driver's own count of display events
(`Gpu::display_events`): the count must have moved and then stood for the
quiet time. A stalled engine puts the asking off, and a dead one ends it.
Nothing here is on the way into secure attention: `paint` and `activate` use
the image `follow` left, which is always a whole one.

**What is asked.** `refit` sends GET_DISPLAY_INFO and GET_EDID, the two
commands `Gpu::probe` sends, and hands their answers and the declaration read
at start to `render::refit`. That is `geometry_for` put beside the screen that
stands, and it answers one of three things (`render::Refit`). `Stands`:
nothing is done and nothing said. `Scale(k)`: the same size at another scale,
so `scale` is set and the next `paint` draws at it. `Size(size, k)`: a new
image, made by `remake`. A device that shows no display (`Ok(None)`) gives
nothing to follow to. A command that was not answered is a failure and is not
read as "no EDID".

**A new image.** `remake` makes the new image beside the one that stands and
swaps only when the device has answered for it whole:

1. The workspace (`Raster`) is replaced by a larger one if the new size needs
   more. The old one is erased and unmapped by its `Drop`, after the new one
   is had.
2. `back(bytes)` adds backing until there is enough. The backing is whole
   1 MiB chunks, chunk `i` mapped at `BASE` plus `i` MiB, so the image is one
   run of memory at any size. A chunk is zeroed when it is mapped and is not
   given back. `MOST` (34) is the chunks of the largest screen and `END` the
   address past them: `back` refuses more, and `backend/gpu/cursor.rs` asserts
   at compile time that its own window begins at or after `END`.
3. The image is in one of two reserved resources (`objects::TRUSTED_IMAGES`;
   `image` says which). The other, the spare, is created at the new size and
   the first chunks of the backing are attached to it. Both resources then
   name the same private memory. Only the image is transferred to, and only
   the image is put on the scanout.
4. When both commands were answered, `image`, `width`, `height`, `scale` and
   `pixels` change together, and the resource that was the image is detached
   and unreferenced.

**A spare whose state is not known.** `spare_owed` is set before the spare's
first command, and only `release_spare` clears it. That detaches and
unreferences the reserved resource that is not the image, and clears the flag
when both commands were answered: the engine live afterwards and no stall
across them (`Gpu::stalls` unchanged). A refusal is an answer: the device has
no such resource, which is what is wanted. It is called in three places. A
making ends with it, for the image that was. The next making begins with it
while the flag is set, and gives up if it is not answered. And `follow` calls
it after any asking that succeeded with the flag still set, so that an image
the device kept is not its memory until the display next changes (round R1
F5). Not answered there, `follow` says
`lictor: trusted screen WxH at Kx: the image before it was not let go of`,
with `, to be tried again` while `Follow::retry` has tries left.

The unreference is `Gpu::unref_own`, not `Gpu::resource_unref`. That one
answers Ok for a release it has put off (`condemn`: a resource the display may
still name, or any resource while its list has overflowed), which would clear
the flag over a resource the device still holds. The display names no trusted
resource while the seat is normal, because restoration's accepted bind drains
the list, so `unref_own` always sends the command. It is no client's command:
it moves neither the command ticks a reply carries (`cmd_seq`,
`last_unref_seq`, which [[sub-tapestryd]]'s presentable self-test orders by;
round R1 F3) nor the journal of retirements clients wait for.

**The stall lever is a client's.** `test_stall_next` arms a shot for the next
synchronous command, and `follow` runs before the pass serves its
connections. It takes an armed shot off the lever while its own commands go
and puts it back after (`test_stall_set_aside`, `test_stall_put_back`; test
builds only), so the shot rides the command its client armed it for (round R1
F2). The seat's commands on entry and the cursor plane's are not set aside.

**A failure** leaves `image`, the size and the scale as they were, so the
screen secure attention enters is the one that stood. `follow` says
`lictor: trusted screen stays WxH at Kx: <why>`, and `, to be tried again`
while `Follow::retry` has tries left (six, at the waits the compositor's
following uses). The next display event asks again in any case. A success
says the line the start says:
`lictor: trusted screen WxH at Kx for a display of WxH`.

**The reserved IDs.** `objects.rs` holds all three: `TRUSTED_IMAGES`
(`TRUSTED_ID_START` plus 1 and plus 3) and `TRUSTED_CURSOR` (plus 2, the
normal cursor's plane, as before), with a compile-time check that they
differ. The first version of this code (f282857b0, never run) took plus 2 for
the second image. The first making would have been refused, and the try after
it would have released the cursor's resource and made the trusted image under
the cursor's ID. It was found by reading, before any guest ran it.

**What it does not do.** It does not follow while a trusted episode is up: a
display that changes then is followed when the seat is normal again, and until
the episode ends the screen is shown as the frontend shows a scanout of
another size. Memory grown for a large display is kept for the rest of the
boot. The backing handed to the device is whole chunks, so up to 1 MiB longer
than the image. The commands block the service's pass as a client's do: a
display that changes delays other work by eight commands at most.

**Tests.** Host:
`render::tests::a_display_that_changed_asks_for_the_screen_it_would_have_had_from_the_start`
(`refit`: the three answers, and that each is `geometry_for`'s). Guest:
`tools/interactive/ls-graphical-sak-follow.exp`, one boot of the session image
on a 2560x1600 display declared at 200, told new sizes as a VNC viewer's
resize tells them. It reads Lictor's line and a dump of the scanout under
secure attention: 1600x1000 at one (the rule on device row 47); a change to
3456x2160 while that screen is up, which must say nothing and leave the
scanout; and after the episode 3456x2160 at two (the rule on rows 94 and 95),
which needs 29 chunks of backing where the boot made 16. Not shown by any
test: a failed making and its retries, the spare that is owed and its release
asked again, the lever's shot set aside, memory that cannot be had, a display
that changes during entry or restoration, and the EDID's millimetres on this
path. `backend/screen.rs` is in no host build, and no lever stalls one of
Lictor's own commands.

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
Trusted IDs occupy a separate reserved range, and `objects.rs` names each one
in it: two for the trusted screen's image and one for the cursor's plane.
Imported backing names a one-shot
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
The same arming gives the compositor six stall shots to aim
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
(`Gpu::req`, while the engine takes no commands). A request the broker
refuses by its own checks before any command is staged is no submission, and
leaves the shot for the next one: a scanout command that names a resource
which is not an ordinary one is such a request (`Device::execute`). The
compositor's display half aims only at binds of a screen it has just made or
of a client's slot, which are not refused there. The shot still rides the next
synchronous command whoever sends it, so a seat takeover landing between the
compositor's aim and its command would take it. That needs test-mode, the
bootarg and the operator's chord, and vote 30 recovers the seat.
`CtxAttachResource` links the attachment in the ledger before the device
answers, and a failure that left nothing (`left_nothing`) unlinks it, so the
resource can still retire. tapestryd aims three at the
compositor context's create, at one blit-convention attempt, and at a
composition health step once GPU composition runs, and three at binds of the
display's, chosen by the height of the host a display event tells
([[sub-tapestryd]], `ls-display-stall`).
Only the designated compositor reaches the broker, and only the booting
operator sets the token. `tools/interactive/ls-gpu-dead.exp` is the gate for
the retirement shots, `tools/warp-host.sh composed-stall` (the GL host) for
the first three stall shots, and `tools/interactive/ls-display-stall.exp` for
the display's.

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
`tools/bake-lictor.py`: repository-owned IBM Plex Sans and Cornucopia, 97
glyphs in each of eight fixed faces, baked once for each scale the screen is
drawn at: 328151 bytes at one, 1310276 at two. No TTF parsing, theme read,
user atlas or GPU text renderer enters the trusted path. The host example
`render-preview` uses the same rasterizer but is not an authorization channel.
Normal OS builds require no Python font tooling.

Layout measures the actual baked glyph advances before painting. It chooses a
compact spacing/type variant if the ordinary layout will not fit; long names
and notices wrap rather than being ellipsized. Invalid semantics, mask counts,
geometry or backdrop length are rejected before any destination pixel changes.
The minimum remains 800x720, in layout pixels. Host tests cover all 127
nonempty capability
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
committed at startup, replaced by a larger one if a display that changes needs
it, and reused for every frame. It never competes with normal
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
- There is no layout under 800x720 layout pixels. A display smaller than that
  is given a screen of twice or more its size (since 2026-11-11; before that it
  refused the whole seat at startup), which the frontend shows scaled down.
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
