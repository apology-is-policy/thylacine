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
audit: hard
guarded-by: []
validated-by: [gate-macrunner-headless, prose]
locks: []
hazards: []
abis: []
design: ["docs/MAC-RUNNER.md"]
created: 2026-11-11
updated: 2026-11-11
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
  `add_client` with protocol `@dbus-display`), and closes QMP again. The
  socket's path is used as given, so a caller with a long path starts the app
  in the socket's directory.
- **The window** is 1280x800 points, titled Thylacine, resizable, and goes
  full screen (Control-Command-F). Command-Q quits; closing the window quits.
- **The picture** is the guest's scanout, letterboxed on black. A whole-number
  scale of one or more is sampled nearest, anything else is filtered. The
  guest's alpha is ignored.
- **What a scanout must be:** pixman `x8r8g8b8` or `a8r8g8b8`; 1 to 16384
  pixels a side; rows a whole number of pixels apart, at least the width and
  at most `i32::MAX` bytes. A mapped one may start anywhere in its file (the
  guest decides where in its resource the frame is), and the file must hold the
  frame up to the last pixel of its last row. A scanout that is none of this
  is refused with a D-Bus error. QEMU acts on that only for `ScanoutMap`: it
  stops sharing its memory with this listener for the rest of the session and
  sends pixels in its messages instead. It also does so when the reply takes
  more than a second.
- **What a rectangle may be:** anything. `Update` and `UpdateMap` are cut to
  the scanout; what falls outside is dropped without an error (QEMU does not
  wait for a reply to either).
- **Keys** go by where they are on the keyboard: the macOS virtual key code to
  QEMU's `qnum` (`keymap.rs`, 119 keys: every key QEMU's own table knows for a
  Mac but `fn`, which has no `qnum`). Two departures from that table, both on
  purpose: the Help position sends Insert, as a PC keyboard labels it; and on
  an ISO keyboard the key left of 1 and the key beside left Shift are
  exchanged, because macOS reports them with each other's codes there.
  Modifiers come from `flagsChanged`; Caps Lock is a press and a release per
  toggle.
- **The pointer** is absolute, in scanout pixels. Over the letterbox bars
  nothing is the guest's: a move, a press and a scroll there send nothing. A
  release is sent exactly when its press was. Buttons, in QEMU's numbers: left
  0, middle 1, right 2, the two side buttons 5 and 6; any other button sends
  nothing. The wheel is buttons 3 (up) and 4 (down): one notch per wheel
  event, and one per ten points of trackpad scroll, at most eight an event.
- **The guest's pointer** is not in the scanout: the guest gives QEMU an image
  and a hot spot (`CursorDefine`) and says whether it shows one (`MouseSet`).
  While the Mac's pointer is over the picture it wears that image, at the size
  a guest pixel has in the view, or nothing when the guest hides its own. A
  guest that never defines a pointer leaves the Mac's arrow as it is. The image
  is 1 to 256 pixels a side and exactly its bytes; one that is not leaves the
  last pointer standing.
- **Everything the guest holds is let go of** (keys, then buttons) when the
  window stops being the key window, when the application stops being active,
  and when a menu opens.
- **The click that activates the window is not sent** to the guest (AppKit's
  `acceptsFirstMouse:` default).
- **A test's arguments** (`view::Options`): `--stats`, `--no-activate`,
  `--exit-after <s>`, `--dump <ppm>` (and, beside it, `<name>-pointer.ppm`:
  what AppKit draws of the pointer over mid-grey), `--scripted-input`; and
  `THYLACINE_NO_MAP=1`, which stops the listener advertising `Unix.Map`.

## Mechanism

**Three threads.** The main thread has AppKit, Metal and the display link. A
`display` thread owns both D-Bus connections (the one the app calls QEMU on,
and the one QEMU calls the listener on) and never leaves `zbus::block_on`. An
`input` thread makes one blocking call per key or pointer event.

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
and a `dirty` flag is set.

**A frame.** Both the upload and the presentation run from a display-link
tick. With the flag set, the main thread uploads the pending rectangle into a
`BGRA8Unorm` texture (`replaceRegion`, straight from QEMU's memory or from the
copy, `stride` bytes a row), draws one triangle that covers the target through
a letterboxed viewport, and presents through a `CAMetalLayer` tagged sRGB. If
the layer has no drawable to give, the frame stays owed and the next tick tries
again. A display link does not tick while the display sleeps (0 ticks in 4 s
with `CGDisplayIsAsleep` true), so nothing that must happen regardless may
hang off it: a test's deadline has its own timer.

**Where the picture is.** One function, `fit`, places a picture in an area:
the drawing takes its viewport from it (in pixels, rounded to whole ones) and
the pointer takes the picture's rectangle from it (in points). A position is
mapped against the texture, which is what is on the glass.

**Keys the guest holds.** A set of held virtual keys decides everything: a
key-up for a key not in the set sends nothing, and a modifier's `flagsChanged`
is read against the set because the flag stays up while the key's twin on the
other side is down. Every key event also says which modifiers are down; a
modifier in the set that the event says is up was released where the view
could not see it, and is released in the guest before the event's own key is
sent. Buttons have the same kind of set.

**The key-up AppKit keeps.** `NSApplication` does not deliver a key-up while
Command is held. The guest saw the key go down, so the app's `NSApplication`
subclass hands that key-up to the event's window. Without it, Command-D
reached QEMU as press 219, press 32, release 219: D stays held in the guest.

**A menu.** While a menu is open AppKit gives the key events to the menu, the
releases with them, and the window stays the key window throughout. So the
app observes `NSMenuDidBeginTrackingNotification` and lets go of everything
then. A key still down when the menu closes is not pressed again in the guest:
the same rule as after any loss of the keys, and the one that keeps a
Command-Tab into the window from reaching the guest as a Super key.

**The pointer the Mac wears.** The display thread keeps the last image and
whether the guest shows it. On a tick, if either changed or the view's scale
did, the main thread makes an `NSCursor` (an `NSBitmapImageRep` of the image
with red and blue exchanged and alpha not multiplied in, sized in points so
that one image pixel covers one guest pixel on the glass) and invalidates the
view's cursor rectangle, which is the picture. The guest's own position for
its pointer is not used: the guest moves it to the positions it is sent, so
the Mac's pointer is already there, without the lag of the round trip.

## Data structures

`display::Shared` (one mutex): the current `Scanout` (`Pixels::Mapped(Arc<Region>)`
or `Pixels::Owned(Vec<u8>)`, width, height, stride, and a serial that changes
with every scanout the guest sets), the pending damage rectangle, the guest's
`Pointer` and whether it is shown, and two counters. `Region` is a mapping
(its base, its length, and where in it the frame starts) and unmaps on drop.
`view::Renderer` holds the Metal objects, the texture and the sizes it was
made for; `view::Worn` the `NSCursor` and what it was made from.

## Concurrency

- The mutex is taken by the display thread (every listener method) and by the
  main thread (the upload, the pointer). A texture for a new scanout is made
  with the lock RELEASED, and the scanout's serial is checked again after:
  making a large one takes long enough that QEMU, waiting on the display
  thread inside `ScanoutMap`, would pass its one-second limit and stop sharing
  its memory. A mapped scanout is copied to the texture outside the lock too,
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
  can know of it: when its release arrives, when the keys are lost or a menu
  opens, and, for a modifier, at the next key event that shows it up. What is
  left: an ordinary key whose release some other part of the system takes
  while the window stays key and no menu opens stays down in the guest until
  the keys are next lost.

## Error paths

- No QMP, no D-Bus display, no Metal device: a message and exit 1. A monitor
  or a display connection that does not answer within 10 s is the same
  (`ATTACH_TIMEOUT`): QEMU answers both from its main loop, and one that is
  hung never would. Until then the launch shows nothing: the attach runs on
  the main thread, before the window and the menu exist.
- A map that is refused is logged, and from then on QEMU sends this listener
  pixels (see Contract).
- An input call QEMU refuses is logged (the first, and then each time the
  count doubles) and the next event is tried. An I/O error means QEMU is
  gone: it is logged and the input thread stops. The display thread stays
  parked and the window keeps its last frame; nothing in the window says so
  yet.
- A file QEMU truncates after the map faults the app on its next read. This is
  inherent to mapping a file a peer owns; the IOSurface scanout
  (`docs/MAC-RUNNER.md` 4.2) replaces the mapping.

## Performance

No figure with a guest yet (`docs/MAC-RUNNER.md` M-3 is the measurement).
Without one, at 640x480: the D-Bus connection is up in 0.8 ms and the first
frame arrives 1.3 ms after the listener registers.

## Prosecution

Prosecute any change to what the listener accepts from QEMU (sizes, strides,
offsets, formats, rectangles, the shared file, the pointer's image), to the
bounds of the copy into the texture, to what is done while the mutex is held
(QEMU waits a second, once, and then stops sharing), and to the bookkeeping of
held keys and buttons (one left down in the guest).

## Seams

- QEMU's D-Bus display (`org.qemu.Display1.*`), an upstream interface; run
  against QEMU 10.0.2 and 11.1.3.
- `tools/run-vm.sh`'s `app` and `dbus` display modes ([[sub-substrate-machine]]).
- The guest sees a virtio keyboard, a virtio tablet and gpu0 through QEMU; the
  app knows nothing of the guest.

## Caveats

- **Not there yet:** resize and HiDPI (`SetUIInfo`), relative pointer motion
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
- **The pointer's colours are not managed.** The picture is tagged sRGB; the
  pointer's image is given to AppKit as device RGB.
- **The input queue has no bound.** Adjacent pointer positions are sent as
  one (`next_input`), so a stalled QEMU costs one stale position, not
  thousands; keys and buttons behind it still queue and are replayed.
- **Not driven by any test:** a scroll (the notch arithmetic has unit tests);
  the middle and side buttons (their numbers have a unit test); the ISO
  exchange on a real keyboard (unit test, and the same rule SDL applies); a
  real menu, a real loss of the keys and a real left click through the window
  (the test posts AppKit's notifications itself and hands the left click to
  the view).
- **Nobody has compared the glass with the guest.** The test compares the
  pipeline's 1:1 output read back from Metal, and what AppKit draws of the
  pointer; whether the panel shows the same is a person's look.
- **The licence field says `GPL-3.0-only`.** Vote M1 says the app is GPLv3;
  "only" against "or later" is the operator's to settle.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
