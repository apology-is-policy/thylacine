---
id: gate-macrunner-headless
type: gate
title: "The Mac app's headless test"
proves: "On each pixel path (the scanout mapped from QEMU's memory; pixels sent in the D-Bus messages): that the Metal pipeline's output at 1:1 is byte-identical to QEMU's own screendump of QEMU's placeholder frame; that a scripted run of AppKit events reaches QEMU as exactly 44 expected lines of its input trace; and that an image defined as the guest defines its pointer is what AppKit draws for the Mac's pointer (six pixels, one of them half-clear)."
blind-to: "A guest: QEMU is paused before its first instruction, so the one frame is its 640x480 grey-on-black placeholder at offset 0 with rows exactly as wide as they are apart -- no colour (channel order of the picture is not shown), no damage after the first frame (partial uploads and in-band Update blits never run), no resize, no pointer the guest defined (the script defines one through the function CursorDefine calls). The panel: the comparisons read back from Metal and from AppKit, not the glass, and a display that is asleep presents nothing. The letterbox as drawn (the picture is read back at 1:1; the crate's unit test draws it at four sizes). AppKit's own decisions: the script posts the lost-keys and menu notifications itself, hands the left click to the view (an inactive window keeps a left click), and its events are made by NSEvent, not by the window server. The scroll wheel, the middle and side buttons, drags, the ISO key exchange on a real keyboard. Whether the binary is the tree's: only that it is not older than the crate's sources."
invocation: "macrunner/app/test/headless.sh (THYLACINE_QEMU, THYLACINE_MAC_APP); about 12 s; a window opens twice for 4 s without taking focus; exit 77 off macOS or when QEMU has no D-Bus display, exit 1 when there is no QEMU or the app is older than its sources. The unit tests: cargo test --release in macrunner/app (20; two need a Metal device)."
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
Option going down and never coming up, then a key whose flags say no modifier
is down (Option is released first); a key held while a menu opens, likewise;
the middle of the view; a key (adjacent positions may be sent as one, and the
script wants both); a right click at guest pixel (10, 10) through the
letterbox; a left click handed to the view; a right click over the bar
(nothing); a right press over the picture released over the bar (the release,
without a position); the left button held while the window loses the keys
(released then, its own release late); a move over the bar (nothing); and
Command-D through `NSApplication` (the key-up AppKit keeps).

The pointer: three opaque colours, white, a clear pixel with a colour that
must not show, and (200, 100, 40) at half strength, which over mid-grey is
(164, 114, 84) when its alpha is applied to it and (255, 164, 104) when the
alpha is taken as applied already.

Evidence stays in `build/macrunner-app-test/<leg>/`.

## What it has and has not caught

- A shader with the picture upside down turns the picture check red (2026-11-10).
- Without the `NSApplication` subclass the input check is red: no `release 32`
  under Command (2026-11-10).
- With every leg forced in-band, the `map` leg is red on the path check
  (2026-11-11).
- Eight fixes switched off one at a time in a scratch copy, each red
  (2026-11-11, QEMU 11.1.3): the pointer's red and blue not exchanged; its
  alpha taken as applied already; the lost-keys notification releasing
  nothing; a key-down not settling the modifiers; a menu opening releasing
  nothing; a press over the bar sent; the left button sent as the middle one;
  buttons not let go of with the keys. The second of these stayed GREEN the
  first time: the image then had no pixel the two readings of alpha disagree
  on (a clear pixel and a half-clear black come out the same either way), and
  the half-clear colour was added for it.
- Three more through the unit tests, each red: the upload given four times the
  width as the distance between rows; a mapping that forgets where in its page
  the frame starts; the picture drawn at the top and not centred.
- The left click through the window is absent from the expected sequence on
  purpose: the window is not active, and AppKit keeps the activating click.
  With `acceptsFirstMouse:` answering YES it arrives.
- A monitor socket that accepts and never answers: the app exits 1 after 10 s
  with the monitor's path in the message (2026-11-11; before the timeout it
  waited for ever). Not a leg of the script.
- It did not find, and could not have found, what reading found: the four
  defects before the first commit, the missing guest pointer, and the twelve
  findings of audit round 1. None was reachable with a well-behaved QEMU, a
  static picture and the script as it then was.
