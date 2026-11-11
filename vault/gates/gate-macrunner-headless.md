---
id: gate-macrunner-headless
type: gate
title: "The Mac app's headless test"
proves: "On each pixel path (the scanout mapped from QEMU's memory; pixels sent in the D-Bus messages): that the Metal pipeline's output at 1:1 is byte-identical to QEMU's own screendump of QEMU's placeholder frame; that a scripted run of AppKit events reaches QEMU as exactly 60 expected lines of its input trace; and that an image defined as the guest defines its pointer is what AppKit draws for the Mac's pointer (six pixels, one of them half-clear)."
blind-to: "A guest: QEMU is paused before its first instruction, so the one frame is its 640x480 grey-on-black placeholder at offset 0 with rows exactly as wide as they are apart -- no colour (channel order of the picture is not shown), no damage after the first frame (partial uploads and in-band Update blits never run), no resize, no pointer the guest defined (the script defines one through the function CursorDefine calls; that the guest's image has its alpha multiplied in was read from its rasteriser, and this test cannot show it). The panel: the comparisons read back from Metal and from AppKit, not the glass, and a display that is asleep presents nothing. The letterbox as drawn (the picture is read back at 1:1; the crate's unit test draws it at four sizes). AppKit's own decisions: the script posts the lost-keys and menu notifications itself, hands the left click to the view (an inactive window keeps a left click), and its events are made by NSEvent, not by the window server. A trackpad's scroll (the script sends one wheel notch, made by Core Graphics and handed to the view), the middle and side buttons, drags, the ISO key exchange on a real keyboard, the application ceasing to be active, the two D-Bus methods that carry the guest's pointer and a pointer the guest hides, the cursor rectangles, any byte order but QEMU's own (the unit tests draw all four), Quit from the menu (the run leaves through terminate: by its own deadline). Whether the binary is the tree's: only that it is not older than the crate's sources."
invocation: "macrunner/app/test/headless.sh (THYLACINE_QEMU, THYLACINE_MAC_APP); about 12 s; a window opens twice for 4 s without taking focus; exit 77 off macOS or when a QEMU that runs has no D-Bus display, exit 1 when there is no QEMU, when it does not run, or when the app is older than its sources. The unit tests: cargo test --release in macrunner/app (31; four drive Metal, and with no Metal device they say so and pass without trying)."
created: 2026-11-11
updated: 2026-11-11
---
## Method

Two legs, `map` and `in-band` (`THYLACINE_NO_MAP=1`). Each starts
`qemu-system-aarch64 -S -nodefaults -display dbus,p2p=on` with a virtio GPU,
tablet and keyboard and the `dbus_*` trace events on, runs the app with
`--attach q.sock --no-activate --exit-after 4 --dump app.ppm --scripted-input`,
then takes a QMP `screendump`. A leg passes when the app exited 0, `app.ppm`
equals `screendump.ppm` byte for byte, the trace's `dbus_kbd_*` and
`dbus_mouse_*` lines are exactly the expected sequence, the app's `finish`
line names the pixel path the leg is for and a pointer of 3x2 with hot spot
(1, 1) that is shown, and `app-pointer.ppm` holds the six expected pixels.

The script (`view.rs`, `scripted_input`; the expected lines are annotated one
case a line in the test): a key, an arrow (an `e0` code), Shift through
`flagsChanged`, a repeat; a key held while the window loses the keys, another
key typed, and the first one's release arriving late (it must send nothing);
Option going down, then a key whose flags say no modifier is down (Option is
released first, and its own release, arriving late, sends nothing); a key
held while a menu opens, likewise; the two Shifts with the keyboard's own bit
for each (left down, the keys lost, right down, left up: a release of a key
the guest no longer holds, which must not read as a press); the middle of the
view; a key (adjacent positions may be sent as one, and the script wants
both); a right click at guest pixel (10, 10) through the letterbox; Control
going down and a click whose flags say no modifier is down (Control is
released before the button goes down); a wheel notch over the same pixel (its
place first); a left click handed to the view; a right click over the bar
(nothing); a right press over the picture released over the bar (the release,
without a position); the left button held while the window loses the keys
(released then, its own release late); a move over the bar (nothing);
Command-D through `NSApplication` (the key-up AppKit keeps); and the left
button going down and staying down to the end, where the app leaves through
`terminate:` and lets go of it.

The pointer: three opaque colours, white, a clear pixel with a colour that
must not show, and (200, 100, 40) at half strength with its alpha multiplied
in, as the guest's image has it, which over mid-grey is (164, 114, 84) when it
is taken so and (114, 89, 74) when the alpha is applied to it a second time.

Evidence stays in `build/macrunner-app-test/<leg>/`.

## What it has and has not caught

- A shader with the picture upside down turns the picture check red (2026-11-10).
- Without the `NSApplication` subclass the input check is red: no `release 32`
  under Command (2026-11-10).
- With every leg forced in-band, the `map` leg is red on the path check
  (2026-11-11).
- Eight fixes switched off one at a time in a scratch copy, each red
  (2026-11-11, QEMU 11.1.3): the pointer's red and blue not exchanged; its
  alpha applied a second time; the lost-keys notification releasing
  nothing; a key-down not settling the modifiers; a menu opening releasing
  nothing; a press over the bar sent; the left button sent as the middle one;
  buttons not let go of with the keys.
- The control on the pointer's alpha was wrong twice. It first stayed GREEN:
  the image had no pixel the two readings of alpha disagree on (a clear pixel
  and a half-clear black come out the same either way), and a half-clear
  colour was added for it. It then pinned the WRONG reading: it was written
  for alpha not multiplied in, which is how QEMU's Cocoa display reads these
  bytes, and the guest multiplies it in. Reading Lictor's rasteriser found
  that; no run of this test could have, because the script's image is made by
  the test. The control now switches the right reading off and shows
  (114, 89, 74).
- The cut of a colour to its alpha is seen by the unit test only: with the
  cut removed, AppKit draws the script's clear pixel the same.
- Three more through the unit tests, each red: the upload given four times the
  width as the distance between rows; a mapping that forgets where in its page
  the frame starts; the picture drawn at the top and not centred.
- Audit round 2's fixes, switched off one at a time the same way (2026-11-11,
  QEMU 11.1.3), each red. Through the script: a click and a move not settling
  the modifiers; a modifier's direction taken from the bit its pair shares; a
  scroll sent without its place; nothing let go of when the app leaves.
  Through the unit tests: the same modifier rule; the pointer's size without
  its bound; a texture installed before the scanout is looked at again; a
  mapped scanout that cannot be shown refused; the colours read in one order
  whatever the frame's; each step of the attach given time of its own; pixels
  in another order copied into the scanout; a scanout in a message that cannot
  be shown leaving the last frame. With the three earlier lists that is 24
  controls, all red on the tree as it now is, and the unchanged copy green.
- One control stayed GREEN and is not counted: with the wait for the queue
  removed from the way out, the release of the held button still reached QEMU
  before the process ended. That the app waits is therefore not shown by the
  script; that the wait works when asked is a unit test.
- The left click through the window is absent from the expected sequence on
  purpose: the window is not active, and AppKit keeps the activating click.
  With `acceptsFirstMouse:` answering YES it arrives.
- A monitor socket that accepts and never answers: the app exits 1 after 10 s
  with the monitor's path in the message (2026-11-11; before the timeout it
  waited for ever). Not a leg of the script.
- It did not find, and could not have found, what reading found: the four
  defects before the first commit, the missing guest pointer, the twelve
  findings of audit round 1, the pointer's alpha, and the nine findings of
  round 2. None was reachable with a well-behaved QEMU, a static picture and
  the script as it then was.
