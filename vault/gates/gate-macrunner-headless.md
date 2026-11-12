---
id: gate-macrunner-headless
type: gate
title: "The Mac app's headless test"
proves: "On each pixel path (the scanout mapped from QEMU's memory; pixels sent in the D-Bus messages): that the Metal pipeline's output at 1:1 is byte-identical to QEMU's own screendump of QEMU's placeholder frame; that a scripted run of AppKit events reaches QEMU as exactly 73 expected lines of its input trace, and that a modifier whose release never came is let go of by a click and by a scroll over the bar, which send nothing and which the view itself reports; and that an image defined as the guest defines its pointer is what AppKit draws for the Mac's pointer (six pixels, one of them half-clear). And on a QEMU with no guest that is read as a guest's driver reads it (qtest): that its virtio GPU answers GET_DISPLAY_INFO and GET_EDID with the size the app told it for the window, in pixels and in the millimetres of as many points, and has its display event pending -- once as the app attaches, and once after the window's content is set to another size in three steps. That size is a mode too large for QEMU's EDID to have a detailed timing, and comes back as whole centimetres. And that a size which changes and comes back to what was told is not told again. And in a window one point larger each way than the scanout has pixels: that the pointer is mapped where a picture at a whole scale is drawn, a click on each bar reaching QEMU as nothing and a click on the first pixel of the middle row and on the last pixel of the last row as those pixels."
blind-to: "A guest: QEMU is paused before its first instruction, so the one frame is its 640x480 grey-on-black placeholder at offset 0 with rows exactly as wide as they are apart -- no colour (channel order of the picture is not shown), no damage after the first frame (partial uploads and in-band Update blits never run), no resize, no pointer the guest defined (the script defines one through the function CursorDefine calls; that the guest's image has its alpha multiplied in was read from its rasteriser, and this test cannot show it). The panel: the comparisons read back from Metal and from AppKit, not the glass, and a display that is asleep presents nothing. The letterbox as drawn (the picture is read back at 1:1; the crate's unit test draws it at five sizes, one of them a pixel past a whole scale). The bar leg reads where clicks land and not the picture at that size, and has run only at two pixels to a point. AppKit's own decisions: the script posts the lost-keys and menu notifications itself, hands the left click to the view (an inactive window keeps a left click), and its events are made by NSEvent, not by the window server. A trackpad's scroll (the script sends one wheel notch, made by Core Graphics and handed to the view), the middle and side buttons, drags, the ISO key exchange on a real keyboard, the application ceasing to be active, the two D-Bus methods that carry the guest's pointer and a pointer the guest hides, the cursor rectangles, any byte order but QEMU's own (the unit tests draw all four), Quit from the menu (the run leaves through terminate: by its own deadline). Whether the binary is the tree's: only that it is not older than the crate's sources. The size leg: a guest (what answers is QEMU's device model; whether Lictor boots at that size, and what scale the guest takes from those millimetres, needs one); a hand on the window's edge, full screen, and a move to another screen (the script sets the content's size three times in one turn of the run loop); a screen with one pixel to a point when the test runs on one with two, and the other way (the unit test has both); which screen the window is on (the app's pixels to a point must be those of SOME display Core Graphics lists); a device that takes no size; how long a size must stand before it is told, and the run-loop mode of that timer (any wait under some three quarters of a second, none included, gives the same two tellings); that the size is told before any input (no leg has both); the offsets (the app sends none, and QEMU's answer to a guest carries no position); that QEMU drops a repeat of a size (the app sends none). The input script: every modifier event in it but the pair of Shifts is made without the keyboard's own bit; the system's own modifier state is not what the script's events carry (the one scroll that must say no modifier is down is made to say so)."
invocation: "macrunner/app/test/headless.sh (THYLACINE_QEMU, THYLACINE_MAC_APP); about 25 s; a window opens five times without taking focus (4 s, 4 s, a moment, 3 s, 3 s; the fourth is 4200 points wide); exit 77 off macOS or when a QEMU that runs has no D-Bus display, exit 1 when there is no QEMU, when it does not run, or when the app is older than its sources. The unit tests: cargo test --release in macrunner/app (42; six are the guest's own, compiled in with usr/lib/libhalcyon/src/scale.rs, which the test of the size told uses as the guest; four drive Metal, and with no Metal device they say so and pass without trying)."
created: 2026-11-11
updated: 2026-11-12
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
the guest no longer holds, which must not read as a press); the two Shifts
again with only the flag they share (both down, the keys lost, left up while
right holds the flag: sent as a press, which is the guess the dossier's
Caveats explain, and let go of when the flag clears); the middle of the
view; a key (adjacent positions may be sent as one, and the script wants
both); a right click at guest pixel (10, 10) through the letterbox; Control
going down and a click whose flags say no modifier is down (Control is
released before the button goes down); a wheel notch over the same pixel (its
place first); a left click handed to the view; a right click over the bar
(nothing); a right press over the picture released over the bar (the release,
without a position); the left button held while the window loses the keys
(released then, its own release late); a move over the bar (nothing);
three modifiers whose release never comes, each let go of by what follows:
Option by a move, Shift by a click over the bar, Control by a scroll over the
bar. The last two send nothing of their own, and the trace would look the
same if the modifier were let go of only by its late release, so after each
the script asks the view what the guest holds and prints it, and the test
wants both lines.
Command-D through `NSApplication` (the key-up AppKit keeps); and the left
button going down and staying down to the end, where the app leaves through
`terminate:` and lets go of it.

The pointer: three opaque colours, white, a clear pixel with a colour that
must not show, and (200, 100, 40) at half strength with its alpha multiplied
in, as the guest's image has it, which over mid-grey is (164, 114, 84) when it
is taken so and (114, 89, 74) when the alpha is applied to it a second time.

The third leg, `size`, draws and types nothing. QEMU is started paused, with
`-accel qtest` and a GPU made 1024x768, a size no window of the test has, and
`as_guest.py` speaks to the device over the qtest socket as a driver would:
it resets it, takes the EDID feature, sets up the control queue in the
machine's memory, and asks `GET_DISPLAY_INFO` and `GET_EDID`; it reads the
display event before the reset and clears it. It is asked three times: before
any app (the control: 1024x768, no event), after a run of the app that ends
0.2 s in (sooner than the app waits after a change of size, so what that run
told it told as it attached), and after a run that sets the window's content
to 4200x700 points in three steps one second in, and a second later a point
wider and back in one turn. The leg wants QEMU's answer to be the view's
points times the pixels to a point, with the millimetres of the points and
the event pending, and the app to have told once and twice: twice already
when the content goes a point wider, and twice at the end. The points of the
second run are the test's own; the pixels to a point must be those of a
display Core Graphics lists. 4200 points are 4096 pixels or more on any
screen, so QEMU's EDID for that run has no detailed timing, and the leg wants
the whole centimetres of the millimetres told (`111x18 cm`); it works out
which form to want by QEMU's own rule, and fails if the second run's size is
not one of the large ones.

The fourth leg, `bar`, is a window again (`--scripted-bar`). One second in,
the content is set one point larger each way than the scanout has pixels
(641x481 for QEMU's placeholder). With a whole number of pixels to a point
that is a view one guest pixel past a whole scale, the most a guest's fitted
mode leaves (`docs/MAC-RUNNER.md` 4.2). Half a second later the script right
clicks four places, worked out from the rule alone and not through the app's
own `place`: a quarter of a point inside the right edge, the first pixel of
the middle row, the last pixel of the last row, a quarter of a point above
the bottom edge. The leg wants the script's own line to say the view was
641x481 at a whole scale, and QEMU's trace to be the two pixels, (0, 240) and
(639, 479), each as a position, a press, a position and a release, and
nothing else.

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
  controls, all red on that tree, and the unchanged copy green.
- One control stayed GREEN and is not counted: with the wait for the queue
  removed from the way out, the release of the held button still reached QEMU
  before the process ended. That the app waits is therefore not shown by the
  script; that the wait works when asked is a unit test.
- The size leg was red before the app told any size: on the commit before,
  QEMU still answered 1024x768 after the app had run (2026-11-11). Its eight
  controls, each red (QEMU 11.1.3): the size not told as the app attaches; a
  change of size not followed; every step of a resize told (four tellings
  where two are wanted); a size told again; points told for pixels; the
  millimetres of the pixels (677x423 for a 1280x800-point window); a size of
  nothing told (the unit test); width and height exchanged on the way to
  QEMU. The two about pixels and points are red in the leg only on a screen
  with two pixels to a point, which this one is; the unit test sees them
  anywhere, so the eight count as ten. With these, 34 controls, all red on
  the tree as it now is, and the unchanged copy green.
- The first run of the size leg is 0.2 s long on purpose. The view's first
  layout is itself a change of size, and the wait after one tells the same
  size a quarter of a second later: in a longer run, removing the telling at
  attach would have stayed green.
- `as_guest.py` had its own control before it was trusted: a device made
  1024x768 read as that; after the probe's `SetUIInfo(338, 211, 0, 0, 2560,
  1600)` it read 2560x1600 and 338x211 mm with the event pending; read again,
  the event was gone (it clears what it reads). Both on a running and on a
  paused QEMU, 10.0.2 and 11.1.3.
- The left click through the window is absent from the expected sequence on
  purpose: the window is not active, and AppKit keeps the activating click.
  With `acceptsFirstMouse:` answering YES it arrives.
- A monitor socket that accepts and never answers: the app exits 1 after 10 s
  with the monitor's path in the message (2026-11-11; before the timeout it
  waited for ever). Not a leg of the script.
- Audit round 3 (2026-11-11, QEMU 11.1.3) found two of the controls above
  weaker than they read. "A click and a move not settling the modifiers"
  removed two settlings at once, and either alone stayed green: a click
  settles twice (in `button`, and again in `pointer`), no move was made with a
  modifier held, and no scroll was either. It is three controls now, each
  red: a move not settling (the trace has the release after the position), a
  click not settling and a scroll not settling (red by the view's own report;
  the trace is the same either way). And "a size told again" was red only
  because AppKit sets the frame once more while the window is set up; the
  script now sets the content a point wider and back after the size has been
  told, and the control is red by that. One more, through the unit tests: the
  guest's own `scale.rs`, which the test compiles in, with its reference
  changed to 72 to the inch. With these, 34 fixes switched off give 37 red results (three are seen by the script and by a unit test) and the unchanged copy is green in both kinds (the full run, 2026-11-11 18:19Z to 18:35Z; before this change it was 34 results from 31 fixes).
- The bar leg, with the whole-scale rule switched off (the picture stretched
  to fill, as it was until 2026-11-12), is red: both bar clicks arrive, as
  (639, 240) and (320, 479), and the other three legs stay green (QEMU
  11.1.3, two pixels to a point). Through the unit tests, ten changes of the
  rule and of the pointer's placement are each red: the rule never or always
  applied, its bound exclusive, the shorter side bounding, either side alone
  asked, the place not floored on either axis or on one, the pointer counted
  from the top, the pointer's place found in points.
- A changed copy of the crate built into the same target directory is the
  same package to cargo: after the red control, `cargo build` in the tree
  called the tree's build fresh and left the changed binary in its place
  (seen by its hash). `cargo clean -p thylacine-mac` before the next build
  put the tree's own back, byte for byte what it was.
- It did not find, and could not have found, what reading found: the four
  defects before the first commit, the missing guest pointer, the twelve
  findings of audit round 1, the pointer's alpha, the nine findings of
  round 2, and the seven of round 3. None was reachable with a well-behaved
  QEMU, a static picture and the script as it then was.
