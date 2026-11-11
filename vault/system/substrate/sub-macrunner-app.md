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
- **What it accepts from QEMU:** pixman `x8r8g8b8` and `a8r8g8b8`; a frame of
  1 to 16384 pixels a side whose stride is at least four times its width; a
  shared file that holds every byte of the frame. Anything else is refused with
  a D-Bus error, and a refused map makes QEMU send the pixels in its messages
  instead.
- **Keys** go by position: the macOS virtual key code to QEMU's `qnum`
  (`keymap.rs`, 101 keys). Modifiers come from `flagsChanged`; Caps Lock is a
  press and a release per toggle. Every key the guest saw go down is released
  when the window stops being the key window.
- **The pointer** is absolute, in scanout pixels; a position over the letterbox
  bars sends nothing. Buttons: left 0, middle 1, right 2. The wheel is buttons
  3 (up) and 4 (down): one notch per wheel event, and one per ten points of
  trackpad scroll, at most eight an event.
- **The click that activates the window is not sent** to the guest (AppKit's
  `acceptsFirstMouse:` default).
- **A test's arguments** (`view::Options`): `--stats`, `--no-activate`,
  `--exit-after <s>`, `--dump <ppm>`, `--scripted-input`; and
  `THYLACINE_NO_MAP=1`, which stops the listener advertising `Unix.Map`.

## Mechanism

**Three threads.** The main thread has AppKit, Metal and the display link. A
`display` thread owns both D-Bus connections (the one the app calls QEMU on,
and the one QEMU calls the listener on) and never leaves `zbus::block_on`. An
`input` thread makes one blocking call per key or pointer event.

**Why the D-Bus thread does nothing else.** QEMU's main loop calls the listener
synchronously (`ui/dbus-listener.c`: the proxies are made with
`*_proxy_new_sync`, `ScanoutMap` is `call_scanout_map_sync`), and zbus 6 runs a
connection on whichever thread is inside `block_on`: it has no thread of its
own. A listener whose thread is asleep stalls the whole VM. Measured without a
guest: 5019 ms to the first frame with the thread asleep, 1.3 ms with it
parked in `block_on`. Input has its own thread for the mirror reason: the
window's thread must never wait for QEMU.

**The scanout arrives one of two ways.** `Listener.Unix.Map.ScanoutMap(fd,
offset, width, height, stride, format)`: the app checks the geometry, checks
with `fstat` that the file holds `offset + height * stride` bytes, and maps it
read-only and shared. Or, when the listener's `Interfaces` property does not
name `Unix.Map` or the map was refused, `Listener.Scanout` and `Update` carry
the pixels and the app keeps its own copy. `UpdateMap` and `Update` name a
damaged rectangle; it is cut to the surface, unioned into one pending rectangle
and a `dirty` flag is set.

**A frame.** On a display-link tick with the flag set, the main thread uploads
the pending rectangle into a `BGRA8Unorm` texture (`replaceRegion`, straight
from QEMU's memory or from the copy), draws one triangle that covers the
target through a letterboxed viewport, and presents through a `CAMetalLayer`
tagged sRGB. If the layer has no drawable to give, the frame stays owed and
the next tick tries again. Nothing but presentation hangs off the display
link, because it does not tick while the display sleeps (0 ticks in 4 s with
`CGDisplayIsAsleep` true).

**Keys the guest holds.** A set of held virtual keys decides everything: a
key-up for a key not in the set sends nothing, losing the key window releases
the whole set, and a modifier's `flagsChanged` is read against the set because
the flag stays up while the key's twin on the other side is down.

**The key-up AppKit keeps.** `NSApplication` does not deliver a key-up while
Command is held. The guest saw the key go down, so the app's `NSApplication`
subclass hands that key-up to the event's window. Without it, Command-D
reached QEMU as press 219, press 32, release 219: D stays held in the guest.

## Data structures

`display::Shared` (one mutex): the current `Scanout` (`Pixels::Mapped(Arc<Region>)`
or `Pixels::Owned(Vec<u8>)`, width, height, stride, and a serial that changes
with every scanout the guest sets), the pending damage rectangle, and two
counters. `Region` is a mapping and unmaps on drop. `view::Renderer` holds the
Metal objects, the texture and the sizes it was made for.

## Concurrency

- The mutex is taken by the display thread (every listener method) and by the
  main thread (the upload). A mapped scanout is copied to the texture OUTSIDE
  the lock, through the main thread's own reference to the mapping, so QEMU's
  main loop is not held for the length of the copy and a scanout replaced
  meanwhile keeps its memory until the copy ends. A copy the app owns is read
  under the lock.
- QEMU writes the shared memory while the app reads it. A frame can therefore
  be torn across an update; the damage that follows repaints it. The app reads
  that memory only through a raw pointer given to Metal, never through a Rust
  reference.
- The input queue is an unbounded channel from the main thread to the input
  thread.

## Invariants enforced

Not section-28 invariants: I-45 leaves the host half trusted, and these are the
app's own defence against a QEMU that sends something wrong.

- Nothing QEMU sends makes the app read outside a buffer it owns or a mapping
  whose file holds the bytes: `geometry` (sizes, stride), `Region::map` (the
  file's length), `Rect::clamp` (every rectangle), the per-row bounds in
  `Update`, and the bound in the upload's copy.
- A key the guest saw go down is seen coming up.

## Error paths

- No QMP, no D-Bus display, no Metal device: a message and exit 1.
- A map that is refused is logged and QEMU falls back to sending pixels.
- QEMU going away is not handled yet: the input thread stops at its first I/O
  error, the display thread stays parked, and the window keeps its last frame.
- A file QEMU truncates after the map faults the app on its next read. This is
  inherent to mapping a file a peer owns; the IOSurface scanout
  (`docs/MAC-RUNNER.md` 4.2) replaces the mapping.

## Performance

No figure with a guest yet (`docs/MAC-RUNNER.md` M-3 is the measurement).
Without one, at 640x480: the D-Bus connection is up in 0.8 ms and the first
frame arrives 1.3 ms after the listener registers.

## Prosecution

Prosecute any change to what the listener accepts from QEMU (sizes, strides,
offsets, formats, rectangles, the shared file), to the bounds of the copy into
the texture, and to the bookkeeping of held keys (a key left down in the guest).

## Seams

- QEMU's D-Bus display (`org.qemu.Display1.*`), an upstream interface; run
  against QEMU 10.0.2 and 11.1.3.
- `tools/run-vm.sh`'s `app` and `dbus` display modes ([[sub-substrate-machine]]).
- The guest sees a virtio keyboard, a virtio tablet and gpu0 through QEMU; the
  app knows nothing of the guest.

## Caveats

- **Not there yet:** resize and HiDPI (`SetUIInfo`), the clipboard, starting
  and stopping QEMU, the XPC helper, the 9P server. `MouseSet` and
  `CursorDefine` are ignored: Halcyon draws its own pointer, and the host's
  arrow is not hidden over the view yet.
- **Pointer motion is not coalesced.** If QEMU stalls, moves queue without
  bound and are replayed afterwards.
- **The wheel has no end-to-end test.** The notch arithmetic has unit tests;
  nothing drives a scroll event through the window.
- **Nobody has compared the glass with the guest.** The test compares the
  pipeline's 1:1 output read back from Metal; whether the panel shows the same
  colours is a person's look.
- **The licence field says `GPL-3.0-only`.** Vote M1 says the app is GPLv3;
  "only" against "or later" is the operator's to settle.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
