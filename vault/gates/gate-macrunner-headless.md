---
id: gate-macrunner-headless
type: gate
title: "The Mac app's headless test"
proves: "That the app's whole display path short of the panel is exact, and that its input path is exact: on each pixel path (the scanout mapped from QEMU's memory; pixels sent in the D-Bus messages) the Metal pipeline's 1:1 output is byte-identical to QEMU's own screendump, and 22 scripted AppKit events reach QEMU as exactly the expected run of key and pointer events."
blind-to: "A guest: QEMU is paused before its first instruction, so the picture is QEMU's 640x480 grey-on-black placeholder -- no colour (channel order is not shown), no damage after the first frame (partial uploads and in-band Update blits never run), no resize. The panel: the comparison reads the pipeline's output back, not the glass, and a display that is asleep presents nothing. The scroll wheel. Real key events from the window server (the script calls sendEvent on the window and on NSApplication)."
invocation: "macrunner/app/test/headless.sh (THYLACINE_QEMU, THYLACINE_MAC_APP); about 12 s; a window opens twice for 4 s without taking focus; exit 77 off macOS or when QEMU has no D-Bus display"
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
`dbus_mouse_*` lines are exactly the expected sequence, and the app's `finish`
line names the pixel path the leg is for.

The script: a key, an arrow (an `e0` code), Shift through `flagsChanged`, a
repeat, a key held while the window loses the keys and released late (the late
key-up must send nothing), the middle of the view and guest pixel (10, 10)
through the letterbox, a move over the bar (must send nothing), and Command-D
through `NSApplication` (the key-up AppKit keeps).

Evidence stays in `build/macrunner-app-test/<leg>/`.

## What it has and has not caught

- A shader with the picture upside down turns the picture check red (2026-11-10).
- Without the `NSApplication` subclass the input check is red: no `release 32`
  under Command (2026-11-10).
- With every leg forced in-band, the `map` leg is red on the path check
  (2026-11-11).
- The left click of the script is absent from the expected sequence on
  purpose: the window is not active, and AppKit keeps the activating click.
  With `acceptsFirstMouse:` answering YES it arrives.
- It did not find, and cannot find, the four defects a reading of the source
  found on 2026-11-11 (a frame of zero or oversized dimensions, a map longer
  than its file, a frame lost when the layer had no drawable, a notch per
  trackpad event): none of them is reachable with a well-behaved QEMU and a
  static picture.
