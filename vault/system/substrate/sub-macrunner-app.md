---
id: sub-macrunner-app
type: sub
title: "The Mac app -- the window that shows the guest (macrunner/app)"
parent: moc-substrate
code:
  - macrunner/app/src/main.rs
  - macrunner/app/src/view.rs
  - macrunner/app/src/display.rs
  - macrunner/app/src/keymap.rs
  - macrunner/app/test/headless.sh
  - macrunner/app/test/as_guest.py
audit: hard
guarded-by: []
validated-by: [gate-macrunner-headless, prose]
locks: []
hazards: []
abis: []
design: ["docs/MAC-RUNNER.md"]
created: 2026-11-11
updated: 2026-11-12
---
## Purpose

The macOS program a person sees: one window that shows gpu0 and sends the
keyboard and the pointer to the guest. It is the first station of
[[arc-mac-runner]] (`docs/MAC-RUNNER.md` 4.2 and 4.5), a Rust crate
(`thylacine-mac`) on AppKit and Metal through `objc2`, with no window toolkit
between. It does not start QEMU yet: it attaches to one that runs with
`-display dbus,p2p=on`, which is what `THYLACINE_DISPLAY=app` and `=dbus` give
([[sub-substrate-machine]]).

## Contract

- **`thylacine-mac --attach <qmp socket>`** connects to QMP, hands QEMU one end
  of a socket pair (`getfd` under the name `thylacine-display`, then
  `add_client` with protocol `@dbus-display`), asks whether there is a device
  named `gpu0`, and keeps the monitor: the pointer's events go through it.
  QEMU's monitor serves one client at a time, so that socket serves nobody
  else while the app runs; a tool that wants QEMU's monitor meanwhile uses
  another (`tools/run-vm.sh` gives two). The socket's path is used as given,
  so a caller with a long path starts the app in the socket's directory.
- **The window** is 1280x800 points, titled Thylacine, resizable down to
  640x400 points, and goes
  full screen (Control-Command-F). Command-Q quits; closing the window quits.
  Either way everything the guest holds is let go of first, and the releases
  are given up to half a second to reach QEMU.
- **The guest is told the window's size** (`Console.SetUIInfo`): the view's
  size in pixels, and the size in millimetres of a screen that many POINTS
  across at 96 to the inch, so that a guest which takes its scale from pixels
  to the inch finds the Mac's own. It is told as the app attaches, before any
  input is sent, and again a quarter of a second after the view's size, or the
  pixels to a point of the screen it is on, last changed. A size that is the
  last one told is not told again. A view with no size, or with one that is
  not a number, is not told at all: there is nothing true to tell, and the
  rounding would make one pixel and one millimetre of it. QEMU keeps the
  size, answers the guest's next question about its display with it, puts the
  millimetres in the EDID (as whole centimetres only, when the mode is too
  large for a detailed timing: Caveats), and raises the device's display
  event. A guest that
  has not started yet finds it when it first asks; a guest that is running
  takes the event and follows the window ([[sub-tapestryd]]), unless its mode
  is pinned.
- **The picture** is the guest's scanout, letterboxed on black. A whole-number
  scale of one or more is sampled nearest, anything else is filtered. The
  scale is the one that fills the view, unless the whole number below it is
  within one of the picture's own pixels of filling it: then the picture is
  drawn at that whole number, on the view's pixels, and what is left is a bar
  (at most the scale's own number of pixels along the side that limits it).
  That is what a guest's fitted mode needs: a view of 3025x1891 gives a mode
  of 1512x945, drawn at 2 with one column and one row of bar, where filling
  would be a scale of 2.0007 and every pixel filtered. The guest's alpha is
  ignored.
- **What a scanout can be:** one of the eight pixman formats with a byte a
  channel, which are the eight a virtio GPU lets a guest choose (the colours
  in four byte orders, each with alpha or with nothing in the fourth byte); 1
  to 16384 pixels a side; rows a whole number of pixels apart, at least the
  width and at most `i32::MAX` bytes. A mapped one may start anywhere in its
  file (the guest decides where in its resource the frame is), and the file
  must hold the frame up to the last pixel of its last row.
- **A scanout that is none of this is taken, and shown as nothing:** the window
  goes black until the guest sets one that can be shown, and one line says why
  (the first time, and then each time the count doubles). It is not left
  showing the frame before, which is no longer the guest's screen and whose
  size would place the pointer. `ScanoutMap` is answered with an error in one
  case only, a mapping that cannot be made (the file does not hold the frame,
  or `mmap` fails): QEMU answers any error, and any reply slower than a
  second, by never offering its memory to this listener again, and sends
  pixels in its messages from then on.
- **What a rectangle may be:** anything. `Update` and `UpdateMap` are cut to
  the scanout; what falls outside is dropped without an error (QEMU does not
  wait for a reply to either).
- **Keys** go by where they are on the keyboard: the macOS virtual key code to
  QEMU's `qnum` (`keymap.rs`, 119 keys: every key QEMU's own table knows for a
  Mac but `fn`, which has no `qnum`). Two departures from that table, both on
  purpose: the Help position sends Insert, as a PC keyboard labels it; and on
  an ISO keyboard the key left of 1 and the key beside left Shift are
  exchanged, because macOS reports them with each other's codes there. A key
  comes up as the place it went down as, whatever keyboard was typed on in
  between. Modifiers come from `flagsChanged`, each by its own left or right
  bit when the event has one; Caps Lock is a press and a release per toggle.
- **The pointer** is absolute: the scanout pixel under the Mac's pointer. It
  goes to the guest through the monitor (`input-send-event`) and not through
  the display connection, as the value a tablet's axis has for that pixel on a
  screen of the scanout's size: the least value the compositor's own division
  brings back to the pixel (`axis`), which is exact for every pixel of every
  screen the app can show. An event is addressed to the display `gpu0`, head
  0. With the tablet bound to that display (`tools/run-vm.sh` binds it under
  `THYLACINE_DISPLAY=app`) a press, a release or a notch reaches the tablet's
  driver as one report with its place. With the tablet bound to nothing QEMU
  hands the place to the tablet and the button to the pointer device whose
  driver came up last, which in a guest of ours is the mouse, and the button
  first: there a press is not kept behind the move to its place. A QEMU with no
  device of that name is found out at the attach, said once, and sent its
  events unaddressed. Over the letterbox bars nothing is the guest's: a move,
  a press and a scroll there send nothing. A release is sent exactly when its
  press was, with its place over the picture and with none off it. Buttons:
  left, middle, right and the two side buttons (the view counts them in
  QEMU's numbers 0, 1, 2, 5, 6); any other button sends nothing. The wheel:
  one notch per wheel event, and one per ten points of trackpad scroll, at
  most eight an event. A notch is the wheel's button going down, with the
  place it was made over, because a window that is not active is still
  scrolled and is sent no moves.
- **The guest's pointer** is not in the scanout: the guest gives QEMU an image
  and a hot spot (`CursorDefine`) and says whether it shows one (`MouseSet`).
  While the window has the keys and the Mac's pointer is over the picture, it
  wears that image, or nothing when the guest hides its own. An image pixel
  takes the room a guest pixel has in the view, up to a pointer of 256 points
  a side: a guest whose screen is magnified further than that gets a pointer
  that is not. A guest that never defines a pointer leaves the Mac's arrow as
  it is. The image is 1 to 256 pixels a side and exactly its bytes, its
  colours already multiplied by its alpha; one that is not an image leaves the
  last pointer standing.
- **Everything the guest holds is let go of** (keys, then buttons) when the
  window stops being the key window, when the application stops being active,
  when a menu opens, and when the app quits. A modifier whose release the app
  never saw is let go of at the next event of any kind (a key, a click, a
  move, a scroll) whose flags show it up.
- **The click that activates the window is not sent** to the guest (AppKit's
  `acceptsFirstMouse:` default).
- **A test's arguments** (`view::Options`): `--stats`, `--no-activate`,
  `--exit-after <s>`, `--dump <ppm>` (and, beside it, `<name>-pointer.ppm`:
  what AppKit draws of the pointer over mid-grey), `--scripted-input`,
  `--resize-to <w>x<h>` (one second in, the window's content is set to that
  many points, in three steps), `--scripted-bar` (one second in, the content
  is set a point larger each way than the scanout has pixels; half a second
  later, right clicks on the two bars and on the picture's pixels beside
  them); and `THYLACINE_NO_MAP=1`, which stops the listener advertising `Unix.Map`.

## Mechanism

**Three threads.** The main thread has AppKit, Metal and the display link. A
`display` thread owns both D-Bus connections (the one the app calls QEMU on,
and the one QEMU calls the listener on) and never leaves `zbus::block_on`. An
`input` thread talks to QEMU an event at a time, each answered before the
next is sent: a key or the window's size is a call on the display connection,
and a position, a button or a notch is one command on the monitor (a run of
positions is sent as its last). One thread sends both kinds, so the guest
sees keys and the pointer in the order they happened.

**Why the pointer goes over the monitor.** QEMU's display connection gives a
console one pointer mode (`ui/input.c`, `qemu_input_is_absolute`): that of the
pointer device bound to the console, and with none bound that of the one whose
driver reached DRIVER_OK last. It refuses `SetAbsPosition` in the one ("Mouse
is not absolute") and `RelMotion` in the other. Lictor brings up the keyboard,
the tablet and then the mouse, so on a machine with the tablet bound to
nothing, which every launcher of ours made until 2026-11-12, each position
the app sent was refused: the app's pointer did nothing in a live guest, and
no test had tried it (the window's test had a QEMU with a tablet and no
mouse, and read a trace line QEMU writes before it decides). Binding the
tablet to the display makes its console absolute whatever came up last, and
would by itself have cured the refusal; it also rules relative motion out on
that connection for good. The monitor's `input-send-event` takes a position
and relative motion alike, at any time. It also takes the axis value as given,
where the display connection makes one of a pixel as `x * 0x7fff / width`,
which the compositor's `ax * (W - 1) / 32767` brings back a pixel short
nearly everywhere (10 of 1280 as 9, the last column as 1277: OPEN-BUGS
2026-11-12T08:49Z, a defect of every QEMU frontend against that line). And
what one command holds reaches a tablet bound to the display as one report:
its driver cannot see a press before the move to its place, and a report QEMU
has no room for (it drops a whole one) takes the press with its place, where
two commands could lose the place and keep the press. That is the bound
machine's alone. With the tablet bound to nothing the one command is two
reports on two devices, and QEMU hands each device its report in the order
the drivers came up in, last first: the mouse's button, then the tablet's
place (seen in QEMU's trace of its queues, 10.0.2 and 11.1.3). Nothing
orders two devices for a guest, so the app does not try to: the launcher
binds the tablet. `axis` is exact only against that one line
of the compositor; a unit test reads `usr/tapestryd/src/main.rs` and fails
when the line is no longer there.

**Why the D-Bus thread does nothing else.** QEMU's main loop calls the listener
synchronously (`ui/dbus-listener.c`: the proxies are made with
`*_proxy_new_sync`, `ScanoutMap` is `call_scanout_map_sync`), so a listener
that is not being served stalls the whole VM. Measured without a guest: 5019 ms
to the first frame when the thread that built the connections slept, 1.3 ms
with it parked in `block_on`. Input has its own thread for the mirror reason:
the window's thread must never wait for QEMU. Both interfaces are served with
`spawn = false`, so one call is finished before the next is looked at: a
scanout and the updates to it only mean something in the order QEMU sent them.

**The scanout arrives one of two ways.** `Listener.Unix.Map.ScanoutMap(fd,
offset, width, height, stride, format)`: the app checks the geometry, checks
with `fstat` that the file holds the frame (`offset + (height - 1) * stride +
4 * width` bytes: the last row has no padding to count, which a frame at the
end of its resource does not have), and maps it read-only and shared from the
page the offset is in. Or, when the listener's `Interfaces` property does not
name `Unix.Map` or a map was refused, `Listener.Scanout` and `Update` carry
the pixels and the app keeps its own copy. `UpdateMap` and `Update` name a
damaged rectangle; it is cut to the surface, unioned into one pending rectangle
and a `dirty` flag is set. An `Update` whose pixels are in another byte order
than the scanout's is not copied in.

**A frame.** Both the upload and the presentation run from a display-link
tick. With the flag set, the main thread uploads the pending rectangle into a
`BGRA8Unorm` texture (`replaceRegion`, straight from QEMU's memory or from the
copy, `stride` bytes a row). The texture is made for the scanout's byte order:
its swizzle says which stored byte each colour is read from, so the bytes are
never rearranged. It then draws one triangle that covers the target through
a letterboxed viewport, and presents through a `CAMetalLayer` tagged sRGB. If
the layer has no drawable to give, the frame stays owed and the next tick tries
again. A display link does not tick while the display sleeps (0 ticks in 4 s
with `CGDisplayIsAsleep` true), so nothing that must happen regardless may
hang off it: a test's deadline has its own timer.

**Where the picture is.** One function, `fit`, places a picture in an area
of pixels, from the top left, as a rectangle of whole pixels inside the area.
At a whole scale that is the picture's size times the scale, the odd pixel of
a bar going to the right and the bottom. Filled, the side that bounds the
scale takes all of the area's and the other the nearest whole number of
pixels. The drawing takes the rectangle as its viewport. The pointer takes it
through `place`, which finds it in the drawable's pixels, as the drawing
does, and gives it in points from the bottom left, as AppKit counts;
`guest_pixel` then maps a point of it to a scanout pixel, each way by the
rectangle's extent over the scanout's, which is what the drawing stretches
the picture by. Until 2026-11-12 the pointer's place was found in points on
its own, which agreed with the drawing only while both filled the view; and
a filled place was not whole pixels: the drawing floored and rounded it and
the pointer did not, so that an edge pixel could be drawn and send nothing,
or be a bar and send a click, and a product a hair over the area's side (480
times 1000/480) put the picture a pixel outside it (audit round 1, F1 and
F2). The whole scale's test is made on products of whole numbers (`w * t <=
k * (t + 1) * tw || h * t <= k * (t + 1) * th`, t the picture's longer
side), so no rounding decides it. It goes by distance alone: a mode pinned in
the guest is drawn whole too where the view comes that close to a multiple of
it. A position is
mapped against the texture, which is what is on the glass: the texture for a
new scanout replaces the old one only once that scanout is known to be the
current one still.

**The size the guest is told.** `UiInfo::of` takes the view's points and the
window's pixels to a point. The pixels come from `pixels`, the one rule the
drawable's size also comes from, so a guest frame of the size told is drawn a
pixel to a pixel (a whole-number scale of one, sampled nearest). The
millimetres are the points times 25.4 over 96, rounded, at least one. The
guest's rule for a scale (`libhalcyon::scale::scale_pct`: for each side, pixels
to the inch over 96 to the nearest quarter, held to 100..200; then the smaller
of the two) gives 200% on a screen with two pixels to a point and 100% on one
with one. The unit test for it compiles the guest's own source in
(`usr/lib/libhalcyon/src/scale.rs`: `parse_edid_mm` reads the size, `scale_pct`
takes the scale) and puts between the app and it a model of the two ways
QEMU's EDID carries a size. It tries every pair of sides from 22 points to two
metres, at one and at two pixels to a point; a side under a centimetre only
against one of no more than 640 points. Under 22 points the rounding of the
millimetres is too much of the side (21 points at two pixels to a point read
175%), and Caveats has the strip. With a guest, `docs/MAC-RUNNER.md` M-3:
2560x1600 told, 200% taken. The telling is an
entry in the input queue (`Input::Size`), so the window's thread does not wait
for QEMU and the size is in order with the keys. `setFrameSize:` and
`viewDidChangeBackingProperties` each put back a one-shot timer (`SETTLE`); when
it runs out the view's size is told if it is not the last one told. The timer
is in the run loop's ordinary mode, and AppKit runs a drag of the window's edge
in another, so a drag should be told where it ends; no test drags.

What QEMU does with it, read in 11.1.3 (`ui/console.c`, `qemu_console_set_ui_info`;
`hw/display/virtio-gpu-base.c`, `virtio_gpu_ui_info`): it drops a repeat of the
same size, and otherwise a timer on the real-time clock, which runs while the
machine does not, hands the size to the device. The device overwrites the size
its `xres` and `yres` properties seeded, answers `GET_DISPLAY_INFO` and
`GET_EDID` from it, and raises `VIRTIO_GPU_EVENT_DISPLAY`. Measured with no
guest by the size leg, on 10.0.2 and 11.1.3 and against a QEMU that is paused:
the device's two answers and the event. That QEMU drops a repeat is read only;
the leg cannot see it, because the app sends none. The offsets (the app sends
0, 0) are not in QEMU's answer to a guest at all.

**Keys the guest holds.** `keymap::Held` maps each key code that is down to
the place it was sent as, and decides everything: a key-up for a code not in
it sends nothing, and a key-up for one that is sends the place that went down
(the place of two codes depends on the keyboard last typed on, and that can
change while a key is held). Buttons have a set of the same kind.

**Modifiers.** `flagsChanged` names the key and gives the flags. The flags
carry the keyboard driver's own bit for each of the eight modifier keys
(`NX_DEVICE*KEYMASK`) below the bit AppKit names for the pair, and the key's
own bit says whether it is down. An event made without those bits has only
the pair's, which stays set while the twin is down, so there a change to a
key that was down is taken as its release, and a change to one the guest does
not hold as a press. That last is a guess while the twin holds the bit
(Caveats). Every event, of any kind, also
says which pairs are down; a modifier the guest holds whose pair the event
shows up was released where the view could not see it, and is released in the
guest before the event's own key or button is sent.

**The key-up AppKit keeps.** `NSApplication` does not deliver a key-up while
Command is held. The guest saw the key go down, so the app's `NSApplication`
subclass hands that key-up to the event's window. Without it, Command-D
reached QEMU as press 219, press 32, release 219: D stays held in the guest.

**A menu.** While a menu is open AppKit gives the key events to the menu, the
releases with them, and the window stays the key window throughout. So the
app observes `NSMenuDidBeginTrackingNotification` and lets go of everything
then. A modifier still down when the menu closes is not pressed again in the
guest: the same rule as after any loss of the keys, and the one that keeps a
Command-Tab into the window from reaching the guest as a Super key. An
ordinary key still down is pressed again by its own repeats.

**Quit.** `applicationShouldTerminate:` lets go of everything and then waits,
for half a second at most, until the input thread has handed QEMU what was
queued (`Input::Sync`, answered in its turn). QEMU lifts the keys itself when
a display client goes away (`ui/dbus-console.c`), and not the buttons; and an
app that is killed runs none of this.

**The pointer the Mac wears.** The display thread keeps the last image and
whether the guest shows it. On a tick, if either changed or the view's scale
did, the main thread makes an `NSCursor` (an `NSBitmapImageRep` of the image
with red and blue exchanged and its alpha taken as multiplied in, sized in
points so that one image pixel covers one guest pixel on the glass, up to the
bound in the Contract) and invalidates the view's cursor rectangles, so that
AppKit asks for them again: one, the picture, with that cursor, or none when
there is no picture or no pointer to wear. Cursor rectangles belong to the key
window, so a window without the keys shows the Mac's arrow. The guest's own
position for its pointer is not used: the guest moves it to the positions it
is sent, so the Mac's pointer is already there, without the lag of the round
trip.

The alpha is multiplied in because Lictor rasterises the pointer so
(`usr/lib/libhalcyon/src/cursor.rs`, `raster`). QEMU copies the resource's
bytes and its D-Bus interface says only "ARGB, 32 bits per pixel"; QEMU's own
Cocoa display reads the same bytes the other way (`kCGImageAlphaFirst`), which
darkens a soft edge. A colour stronger than its alpha, which such an image
cannot hold, is cut to the alpha before AppKit sees it.

## Data structures

`display::Shared` (one mutex): the current `Scanout` (`Pixels::Mapped(Arc<Region>)`
or `Pixels::Owned(Vec<u8>)`, width, height, stride, its byte `Order`, and a
serial that changes with every scanout the guest sets), the pending damage rectangle, the guest's
`Pointer` and whether it is shown, and two counters. `Region` is a mapping
(its base, its length, and where in it the frame starts) and unmaps on drop.
`view::Renderer` holds the Metal objects, the texture and the size and byte
order it was made for; `view::Worn` the `NSCursor` and what it was made from;
`keymap::Held` the keys that are down. `display::UiInfo` is a size as the
guest is told it (millimetres and pixels); the view keeps the last one told,
how many were, and the timer that waits for the size to stand.

## Concurrency

- The mutex is taken by the display thread (every listener method) and by the
  main thread (the upload, the pointer). A texture for a new scanout is made
  with the lock RELEASED, and installed only if the scanout's serial is still
  the same after: QEMU waits on the display thread inside `ScanoutMap` for a
  second at most and then stops sharing its memory, and what an allocation
  costs is kept out of that wait (the cost itself is not measured). If the
  guest set another scanout meanwhile, the old texture stays on the glass and
  the next tick starts again. A mapped scanout is copied to the texture outside the lock too,
  through the main thread's own reference to the mapping, so a scanout
  replaced meanwhile keeps its memory until the copy ends. A copy the app owns
  is read under the lock.
- QEMU writes the shared memory while the app reads it. A frame can therefore
  be torn across an update; the damage that follows repaints it. The app reads
  that memory only through a raw pointer given to Metal, never through a Rust
  reference.
- A frame is presented without waiting for the GPU, and the next upload writes
  the same texture. A frame still being drawn can show part of the next one's
  pixels; the next frame is whole.
- The input queue is an unbounded channel from the main thread to the input
  thread, which sends a run of adjacent pointer positions as its last one.

## Invariants enforced

Not section-28 invariants: I-45 leaves the host half trusted, and these are the
app's own defence against a QEMU that sends something wrong.

- Nothing QEMU sends makes the app read outside a buffer it owns or a mapping
  whose file holds the bytes: `geometry` (sizes, stride, the frame's length),
  `Region::map` (the file's length), `Rect::clamp` (every rectangle), the
  per-row bounds in `Update`, the bound in `replace` (the upload's copy), and
  `Pointer::new` (the pointer's image). Metal reads the rectangle it is given
  and no byte past its last pixel, rows further apart than they are wide
  included: measured, by a test that puts an unreadable page there.
- A key or a button the guest saw go down is seen coming up, as far as the app
  is told: when its release arrives, when the keys are lost, a menu opens or
  the app quits, and, for a modifier, at the next event of any kind that shows
  it up. What is left: an ordinary key or a button whose release some other
  part of the system takes while the window stays key and no menu opens stays
  down in the guest until the keys are next lost; and an app that is killed
  lets go of nothing (QEMU lifts the keys itself then, and not the buttons).

## Error paths

- No QMP, no D-Bus display, no Metal device: a message and exit 1. An attach
  that is not done within 10 s is the same (`ATTACH_TIMEOUT`, one deadline for
  the monitor's greeting, its four answers and the display connection
  together; after the attach a command on the monitor waits for as long as
  QEMU takes): QEMU
  answers all of them from its main loop, and one that is hung never would.
  Until then the launch shows nothing: the attach runs on the main thread,
  before the window and the menu exist.
- A mapping that cannot be made is logged, and from then on QEMU sends this
  listener pixels (see Contract). A scanout that cannot be shown is logged and
  the window is blank.
- A texture that cannot be made leaves the window blank; each later damage
  tries again.
- An event QEMU refuses, a call on the display connection or a command on
  the monitor, is logged with QEMU's reason (the first, and then each time
  the count doubles) and the next event is tried. The monitor refuses the
  pointer while the machine is stopped ("VM not running") and when the guest
  has no device for it. An I/O error on either connection means QEMU is gone:
  it is logged and the input thread stops. The display thread stays parked
  and the window keeps its last frame; nothing in the window says so yet.
- A console whose device takes no size answers `SetUIInfo` with an error. It
  is counted and logged with the other calls QEMU refuses, and nothing else
  changes.
- A file QEMU truncates after the map faults the app on its next read. This is
  inherent to mapping a file a peer owns; the IOSurface scanout
  (`docs/MAC-RUNNER.md` 4.2) replaces the mapping.

## Performance

With a guest (`docs/MAC-RUNNER.md` M-3; 2026-11-11, an M2 Air, QEMU 11.1.3
under HVF, the scanout 2560x1600 and mapped): an upload averages 1.2 ms and
takes 3.6 ms at worst in a settled session (87 frames), a frame 1.4 ms;
pixels sent in the messages, 1.0 ms over smaller rectangles. Across a guest's
boot the average was 9.2 ms and one upload took 120 ms, which is not
explained. From a key to the guest's next damage: 7.5 to 42 ms, median 11.7,
over 18 keys. The connection is up in 2.4 ms and the first scanout arrives
0.9 ms after the listener registers (0.8 and 1.3 ms with no guest, at
640x480). Not measured: the CPU the app and QEMU use, a stream of full frames
at the display's rate, the making of a texture.

## Prosecution

Prosecute any change to what the listener accepts from QEMU (sizes, strides,
offsets, formats and their byte orders, rectangles, the shared file, the
pointer's image), to what `ScanoutMap` answers with an error (QEMU stops
sharing for good), to the bounds of the copy into the texture, to what is done
while the mutex is held (QEMU waits a second, once, and then stops sharing),
to the bookkeeping of held keys and buttons, the way out included (one
left down in the guest), and to what the guest is told of the window: a size
of nothing, were one sent, turns the guest's display off, and a size told at
every step of a drag is a mode the guest sets at every step.

## Seams

- QEMU's D-Bus display (`org.qemu.Display1.*`), an upstream interface; run
  against QEMU 10.0.2 and 11.1.3.
- QEMU's `qtest` accelerator and protocol, which the size leg's `as_guest.py`
  speaks in a guest's place (a virtio-pci driver of a hundred lines: reset,
  features, one queue, two commands).
- `tools/run-vm.sh`'s `app` and `dbus` display modes ([[sub-substrate-machine]]).
- The guest sees a virtio keyboard, a virtio tablet and gpu0 through QEMU; the
  app knows nothing of the guest.

## Caveats

- **Not there yet:** relative pointer motion
  and capturing the pointer (`docs/MAC-RUNNER.md` 4.2), the clipboard, starting
  and stopping QEMU, the XPC helper, the 9P server, a window that says it is
  connecting or that QEMU has gone.
- **Where it departs from the design's text.** `docs/MAC-RUNNER.md` 4.2 has the
  mapped scanout uploaded into an IOSurface-backed texture; the app uses an
  ordinary texture and `replaceRegion`, and the IOSurface arrives with the
  fork's scanout. 4.5 names the `qemu-display` crate; the app makes its few
  calls itself (`call_method`) and serves the listener with `zbus::interface`.
- **The path with pixels in the messages is the lesser one.** Read in zbus
  6.2.1 and QEMU 11.1.3, not run: a frame of more than 64 MiB cannot be
  received, one of more than 128 MiB ends the listener's connection, and after
  the guest changes its scanout's size QEMU sends no whole frame until the
  guest next flushes one, so an `Update` in between is cut to the old size.
  None of it applies while the scanout is mapped.
- **A window the guest cannot make a mode of its own size for.** The size
  told is the window's, whatever it is, and the guest takes a mode that fits
  it ([[sub-tapestryd]], [[sub-lictor]]): the size itself where a screen and
  a full-screen surface of it can be made, else the size over a whole number,
  up to four. The picture is then smaller than the view and is drawn at that
  whole number, a guest pixel to a square of the view's, with the division's
  remainder as a bar. The bound is the kernel's 64 MiB a weave: three buffers
  of a full-screen surface pass it at 5.59 megapixels. This app's default
  window is under it; full screen on a 14- or 16-inch MacBook Pro is over and
  gets half as many pixels a side (measured 2026-11-11: told 3456x2160, a
  session at 1728x1080, where the guest before had no session and no
  display). Lictor's trusted screen takes the size times or over a whole
  number, so that its renderer can draw it: not under 800x720, not over
  4096x2160 pixels in all (measured: 640x400 told gives 1280x800, 5120x2880
  gives 2560x1440; before, each gave a guest with no compositor, and Lictor
  is the keyboard and the pointer too). The window's least content, 640x400
  points, is that small on a screen with one pixel to a point. A guest that
  is running follows the window: the scanout had the new size 214 ms and
  282 ms after the size was told, one sample each, 150 ms of which is the
  time the guest waits for the size to stop changing. A mode pinned in the
  guest (`mode W H`, which refuses what it cannot make, and more than
  3840x2160) stands whatever the window does; the scale stops at 200%.
- **A large view's EDID has whole centimetres and no preferred timing.** QEMU
  is told no refresh rate and takes 75 Hz, and its EDID gets no detailed
  timing when a side is 4096 pixels or more, or the pixel clock would pass the
  field's 655.36 MHz, which is from 6.25 megapixels: about 1582x989 points on a
  screen with two pixels to a point (full screen on a 16-inch MacBook Pro, an
  iMac, a Studio Display), 3164x1977 on a screen with one. The base block then
  has the size in whole centimetres, cut and not rounded, and the mode only in
  an extension block. The scale a guest takes is still the Mac's: the unit
  test covers those sizes, and the size leg reads such a block from QEMU
  (4200x700 points told, `111x18 cm` answered, on 10.0.2 and 11.1.3). What
  looks for a preferred mode in the base block finds none. A strip, under a
  centimetre one way and that long the other, has no centimetres at all and
  gives a guest no size; no window is that shape (the least is 640x400
  points).
- **A modifier from a source that gives it no bit of its own.** An event made
  by a remapper or a test can carry only the flag a pair shares. While one key
  of the pair holds that flag, a change to the other that the guest does not
  hold is sent as a press. It is a guess. The same event is the release of a
  key the guest was made to let go of (the window lost the keys while both
  were down), and then the guest sees a press where there was a release; it
  ends when the flag clears, and the script has the case. The other guess
  loses a press that was real, and a key that does not work is the worse of
  the two. Asking the system which keys are down was weighed and not taken:
  its answer is of the moment it is asked and not of the event, and whether it
  follows events made without the bits could only be tried by posting keys
  into a person's session. The mirror: a key from such a source going down
  while its twin on a real keyboard is down has the pair's bits and not its
  own, which is what a release looks like, and is read as one; the guest
  still holds the twin.
- **The pointer's colours are not managed.** The picture is tagged sRGB; the
  pointer's image is given to AppKit as device RGB.
- **The input queue has no bound.** Adjacent pointer positions are sent as
  one (`next_input`), so a stalled QEMU costs one stale position, not
  thousands; keys and buttons behind it still queue and are replayed.
- **The pointer has not been tried in a live guest.** What is shown is with a
  QEMU that has no guest and a scripted driver in its place: the reports the
  tablet's driver is handed, on QEMU 10.0.2 and 11.1.3. That the compositor
  takes a button from the tablet was read in its source
  (`usr/tapestryd/src/main.rs`, the tablet's drain), not seen.
- **On a machine whose tablet is bound to nothing a press can be seen before
  its place.** The button's report is the mouse's and is handed over before
  the tablet's with the place, and a compositor that reads its devices
  between the two acts on the press where the pointer was. It matters only
  for a press at a place the pointer was not already at (a move is sent for
  every move of the Mac's pointer over the picture, so in use it nearly
  always was). The app does not find out whether the tablet is bound and
  says nothing of it; `tools/run-vm.sh` binds it under `THYLACINE_DISPLAY=app`,
  and `--attach` to any other machine has this.
- **The monitor is the app's while it runs**, and a stopped machine refuses
  the pointer: the app logs it and the window says nothing.
- **`THYLACINE_ECHO_POINTER=1`** prints each place the guest says its pointer
  is at (`MouseSet`), for a test that wants to see where an event landed.
- **Not driven by any test:** a trackpad's scroll (the notch arithmetic has
  unit tests; the script sends one wheel notch); the middle and side buttons
  (their names have a unit test); the ISO exchange on a real keyboard, and
  the call that asks which keyboard it is (unit test of the rule, the same one
  SDL applies); a real menu, a real loss of the keys and a real left click
  through the window (the test posts AppKit's notifications itself and hands
  the left click to the view); the application ceasing to be active; the two
  D-Bus methods that carry the guest's pointer (the script calls what they
  call, and sets "shown" itself, so a method that did nothing would pass); the
  pointer a guest hides; the cursor rectangles and `NSCursor.set`, the bound
  on the pointer's size as it is applied, and the rectangle going when the
  picture does; a scanout in any byte order but QEMU's own from a real QEMU
  (a unit test draws all four through Metal); Quit from the menu (the test
  leaves through `terminate:` by its own deadline); a hand on the window's
  edge (the script sets the content's size three times in one turn of the run
  loop, so that a drag is told only where it ends is read, not run), full
  screen (how many sizes the transition goes through, and whether any stands
  a quarter of a second, is not known), and a move to a screen with other
  pixels to a point; Caps Lock (one event a toggle, sent as a press and a
  release); Sticky Keys and a remapped Caps Lock, whose events have not been
  looked at; how long the size must stand and which run-loop mode its timer
  is in (the size leg passes with any wait under some three quarters of a
  second, none included); that the size is told before any input (no leg has
  both); a console whose device takes no size; the half second the releases
  are given at quit (that the wait ends when asked has a unit test). An
  upload of part of a frame, after the first, has no unit test and no
  headless leg (QEMU's placeholder is one frame); the guest run of M-3 drove
  it for 87 frames and the picture was QEMU's.
- **Not seen with a guest:** a fitted mode shown at its whole scale. No guest
  has run since the rule was written. It is in the unit tests, on Metal off
  screen (every device pixel of 192 views against the pixel a click on it
  sends, and 27 filled views for which pixels are picture), and in one window
  at two pixels to a point, where the bars are a pixel on every side: AppKit
  made 641x481 points of a content size of 640.5x480.5 (macOS 27.2.1), so a
  window there has no odd pixel to place. Never in a window at one.
- **Two keyboards of different kinds** are read through one call that answers
  for the keyboard last typed on. A key is sent as the place it went down as,
  so it cannot stick; which of two places a key pressed on the other keyboard
  is can still be wrong for that one key.
- **Nobody has compared the glass with the guest.** The test compares the
  pipeline's 1:1 output read back from Metal, and what AppKit draws of the
  pointer; whether the panel shows the same is a person's look.
- **The licence field says `GPL-3.0-only`.** Vote M1 says the app is GPLv3;
  "only" against "or later" is the operator's to settle.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
