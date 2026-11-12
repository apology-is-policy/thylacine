---
id: gate-macrunner-headless
type: gate
title: "The Mac app's headless test"
proves: "On each pixel path (the scanout mapped from QEMU's memory; pixels sent in the D-Bus messages): that the Metal pipeline's output at 1:1 is byte-identical to QEMU's own screendump of QEMU's placeholder frame; that a scripted run of AppKit events reaches a QEMU whose input devices a scripted driver has brought up in our guest's order, the tablet bound to the display, as exactly 62 expected keys and pointer reports (each press, each release over the picture and each notch one report with its place), with nothing refused, and that a modifier whose release never came is let go of by a click and by a scroll over the bar, which send nothing and which the view itself reports; and that an image defined as the guest defines its pointer is what AppKit draws for the Mac's pointer (six pixels, one of them half-clear). And on a QEMU with no guest that is read as a guest's driver reads it (qtest): that its virtio GPU answers GET_DISPLAY_INFO and GET_EDID with the size the app told it for the window, in pixels and in the millimetres of as many points, and has its display event pending -- once as the app attaches, and once after the window's content is set to another size in three steps. That size is a mode too large for QEMU's EDID to have a detailed timing, and comes back as whole centimetres. And that a size which changes and comes back to what was told is not told again. And in a window one point larger each way than the scanout has pixels: that the pointer is mapped as for a picture at a whole scale (the leg does not look at what is drawn), a click on each bar reaching QEMU as nothing and a click on the first pixel of the middle row and on the last pixel of the last row as those pixels. And what the input devices then handed their driver: with the tablet bound to the display, every click on the tablet as one report of a place and a button, and nothing on the mouse; with the tablet bound to nothing, where QEMU's display connection refuses a position (the leg asks QEMU that the pointer device it goes by is the mouse), the places on the tablet and the buttons on the mouse, each click's place sent once ahead of its press, and the app saying once what QEMU answered for the tablet's binding. And that the paced pointer script a running guest is tried with (--scripted-click) sends what it says: the middle, a left click on a pixel and the last pixel, as four reports on the bound tablet, and that the place QEMU gives for a pointer is printed. And that the same script against that machine STOPPED reaches nothing: QEMU refuses three events, not four (the release of a press it refused is not sent), and the app still finds the display and the tablet's binding. And of the captured pointer (--scripted-capture, in a window that is not key), on the machine with the tablet bound and on the one with it bound to nothing: that two moves, a left press between them and its release, a notch and a right press reach the guest's mouse, addressed to no display, and its tablet nothing of them, each move as the whole guest pixels its points make at the view's scale with what is left of a pixel carried into the next (worked out by the leg from the points the script says it moved: 30,-15 and -11,5 at five thirds of a point to a pixel, a scale the leg requires of the view and does not take from it); that the right button, held when the pointer is given back, comes up on the mouse; that what was made before the pointer changed hands and is taken after is not read as the new state's (a move made free and taken captured sends nothing, a move made captured and taken free sends nothing, and a press and a notch made captured and taken free arrive with no place); that a click after that is the tablet's, with its place; that a captured pointer is given back when the window moves, the click after it being the tablet's; and that the picture read back while the pointer is captured is QEMU's screendump but for the guest's pointer, drawn with its hot spot on the place the guest was said to have it, its clear pixel showing the picture and its pixel at half strength blended once. And that the captured pointer's script for a guest that is running (--scripted-captured-click) sends what it says: the place on the tablet, then, addressed to no display, the motion its whole points make and a left click, on the mouse."
blind-to: "A guest: QEMU has none to run (it takes a tester's reads and writes in a guest's place), so the one frame is its 640x480 grey-on-black placeholder at offset 0 with rows exactly as wide as they are apart -- no colour (channel order of the picture is not shown), no damage after the first frame (partial uploads and in-band Update blits never run), no resize, no pointer the guest defined (the script defines one through the function CursorDefine calls; that the guest's image has its alpha multiplied in was read from its rasteriser, and this test cannot show it). The panel: the comparisons read back from Metal and from AppKit, not the glass, and a display that is asleep presents nothing. The letterbox as drawn (the picture is read back at 1:1; the crate's unit test draws it at five sizes, one of them a pixel past a whole scale). The bar leg reads where clicks land and not the picture at that size, and has run only at two pixels to a point. AppKit's own decisions: the script posts the lost-keys and menu notifications itself, hands the left click to the view (an inactive window keeps a left click), and its events are made by NSEvent, not by the window server. A trackpad's scroll (the script sends one wheel notch, made by Core Graphics and handed to the view), the middle and side buttons, drags, the ISO key exchange on a real keyboard, the application ceasing to be active, the two D-Bus methods that carry the guest's pointer and a pointer the guest hides, the cursor rectangles, any byte order but QEMU's own (the unit tests draw all four), Quit from the menu (the run leaves through terminate: by its own deadline). Whether the binary is the tree's: only that it is not older than the crate's sources. The size leg: a guest (what answers is QEMU's device model; whether Lictor boots at that size, and what scale the guest takes from those millimetres, needs one); a hand on the window's edge, full screen, and a move to another screen (the script sets the content's size three times in one turn of the run loop); a screen with one pixel to a point when the test runs on one with two, and the other way (the unit test has both); which screen the window is on (the app's pixels to a point must be those of SOME display Core Graphics lists); a device that takes no size; how long a size must stand before it is told, and the run-loop mode of that timer (any wait under some three quarters of a second, none included, gives the same two tellings); that the size is told before any input (no leg has both); the offsets (the app sends none, and QEMU's answer to a guest carries no position); that QEMU drops a repeat of a size (the app sends none). The input script: every modifier event in it but the pair of Shifts is made without the keyboard's own bit; the system's own modifier state is not what the script's events carry (the one scroll that must say no modifier is down is made to say so). The bar leg: which side an odd pixel of a bar goes to and the turn from AppKit's way up to the guest's (its bars are even at two pixels to a point); a filled picture; one pixel to a point; an odd pixel in a window (AppKit gives a window none at two pixels to a point). The pointer in a live guest: the driver is the test's own script, a device has 64 buffers and is read after the app has left, so only the bar legs' eight reports are read from the devices, and the long script's are read from QEMU's input layer, which does not say which device took them; that the compositor takes a button from a tablet; with the tablet bound to nothing, the order the two devices are handed their reports in (the mouse's button before the tablet's place: seen once by hand in QEMU's trace of its queues, and no leg reads it); relative motion; a machine that is stopped and then continued (that a release QEMU refused reaches the guest once it runs, before anything later, is the unit tests', against a scripted monitor that answers in lines taken from QEMU 10.0.2 and 11.1.3; no leg continues a machine under the app); keys while a machine is stopped; a tablet that is bound and has another name than tab-pci0; a QEMU whose display has another name than gpu0 beyond the app saying so; a script run with the Mac's display asleep. The captured pointer's Mac side: the legs' window is not key, so nothing hides, holds or moves the Mac's pointer, no move is dropped after a jump, and no capture is refused for a pointer that could not be held; the chord, the menu item and the title; a capture ended by the window losing the keys or by a menu, or by the window changing size or screen or entering or leaving full screen (the script moves the window, the one of those five with a leg); the rule for an event made before a change of hands as the Mac's own events meet it (the script stamps its events itself, on the clock the rule reads, and that the Mac stamps a real event on that clock is shown by nothing here); motion a device made (the script hands the view events Core Graphics made, with the motion written into them); motion summed behind a slow QEMU, and the place sent ahead again after motion (the unit tests); the guest's pointer over the bars or at any scale but 1:1 (a unit test draws it at two scales, at a corner and over a bar); a guest that moves its pointer in answer and says where it is (the script says the place itself)."
invocation: "macrunner/app/test/headless.sh (THYLACINE_QEMU, THYLACINE_MAC_APP); about 38 s; a window opens eleven times without taking focus (4 s, 4 s, a moment, then eight times 3 s; the fourth is 4200 points wide); exit 77 off macOS or when a QEMU that runs has no D-Bus display, exit 1 when there is no QEMU, when it does not run, or when the app is older than its sources. The unit tests: cargo test --release in macrunner/app (70; one reads usr/tapestryd/src/main.rs for the line the pointer's axis value is exact against; six are the guest's own, compiled in with usr/lib/libhalcyon/src/scale.rs, which the test of the size told uses as the guest; four drive Metal, and with no Metal device they say so and pass without trying)."
created: 2026-11-11
updated: 2026-11-12
---
## Method

**The machine.** Every leg but `size` starts `qemu-system-aarch64 -accel qtest
-nodefaults -display dbus,p2p=on` with a virtio GPU named `gpu0`, a keyboard,
a tablet named `tab-pci0` as the launcher names it, and a mouse, and the
`dbus_*` and `input_event_*` trace events on.
The machine has no guest and, but in `stopped`, is not paused: QEMU's monitor
refuses input for a machine that is stopped. `as_input_guest.py up` then brings each input
device up over the qtest socket as a driver does (features, an event queue of
64 buffers, DRIVER_OK), in slot order, which is the order Lictor brings them
up in; the leg wants it to answer keyboard, tablet, mouse. That leaves the
mouse the pointer device whose driver came up last. The tablet is bound to
the display (`display=gpu0,head=0`), as `tools/run-vm.sh` binds it for the
app, in every leg but `bar-unbound`.

**How input is read** (`seen` in the script). A key is the display
connection's own trace line (`dbus_kbd_press keycode 30`). The pointer is what
QEMU's input layer was handed (`input_event_abs`, `_btn`, `_sync`), gathered
into the reports it was handed as: `at 10,10 press right` is one report, and
the pixel is what the compositor's arithmetic makes of the two axis values on
a 640x480 screen (`value * (side - 1) / 32767`, rounded down), so the test
knows the guest's rule and not the app's. A report that is not for console 0
says so. Any call on the display connection's pointer interface appears as
its own trace line, which no expectation has. The app must not have logged a
refusal, nor that `gpu0` is no display of this QEMU, nor that the tablet is
not bound to it (`said_of_qemu`); `bar-unbound` wants that last line, once
and with QEMU's answer, and `stopped` has its own account of what was said.

Two legs, `map` and `in-band` (`THYLACINE_NO_MAP=1`), run the app with
`--attach q.sock --no-activate --exit-after 4 --dump app.ppm --scripted-input`
and then take a QMP `screendump`. A leg passes when the app exited 0, `app.ppm`
equals `screendump.ppm` byte for byte, what reached QEMU is exactly the
expected run of keys and reports, the app's `finish`
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
`-accel qtest` and a GPU with no name made 1024x768, a size no window of the
test has (the app must say that `gpu0` is no display of it, with QEMU's
reason, `Device 'gpu0' not found`), and
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
that is a view one guest pixel past a whole scale: the bound of the rule
(`docs/MAC-RUNNER.md` 4.2), and more than a guest's fitted mode leaves, which
is under a guest pixel. At two pixels to a point its bars are one pixel on
every side, so the leg cannot see which side an odd pixel goes to or the turn
from AppKit's way up to the guest's: of the unit tests' mutations of the rule,
six would pass this leg alone (audit round 1, F3). Half a second later the script right
clicks four places, worked out from the rule alone and not through the app's
own `place`: a quarter of a point inside the right edge, the first pixel of
the middle row, the last pixel of the last row, a quarter of a point above
the bottom edge. The leg wants the script's own line to say the view was
641x481 at a whole scale, and what reached QEMU to be the two pixels, (0, 240)
and (639, 479), each as a press and a release with its place, and nothing
else. Then `as_input_guest.py reports 640 480` reads what each device handed
its driver over the leg, a pointer device's as the compositor reads them, and
the leg wants the same four reports on the tablet and nothing on the mouse or
the keyboard.

The fifth leg, `bar-unbound`, is the fourth on a machine whose tablet is bound
to nothing, as it is under every launcher but the app's. Before the app
attaches the leg asks QEMU (`query-mice`) that the pointer device it goes by
is one that is not absolute: the state in which QEMU's display connection
refuses a position, and the app's pointer did nothing until 2026-11-12. The
app asks QEMU at the attach what the tablet is bound to, and the leg wants it
to have said once that it is not the display, with QEMU's answer (`its
display is ""`). From then on a place the guest was not sent yet goes ahead
of its button, so the leg wants QEMU to have been handed six reports with
nothing refused (each click's place alone, then its press and its release
with the place), and the devices to have handed their driver six places on
the tablet and the two presses and releases on the mouse: QEMU's own routing,
said so that a change in it is seen.

The sixth leg, `click`, is on the first machine again and runs the script
none of the others does: `--scripted-click 600,108`, the pointer script for a
guest that is running (the middle of the picture, a left click on a pixel,
the last pixel, half a second apart, handed to the view itself). The leg
wants QEMU to have been handed `at 320,240`, the press and the release each
`at 600,108`, and `at 639,479`; the tablet's driver to have been handed those
four reports and the mouse's nothing; and the script to have said each step
as the pixel it meant, which it does only if that is the pixel the view sent
for it, and which is what a run with a guest compares the guest's own account
with. That account is what `THYLACINE_ECHO_POINTER=1` prints, so the leg runs
with it and wants the one place QEMU gives for a pointer when no guest moves
one (`0,0, shown 0`, as the app registers) printed once.

The seventh leg, `stopped`, is the sixth on the same machine started paused
(`-S`; its devices are brought up all the same). QEMU's monitor refuses the
pointer for a stopped machine, and refuses it after it has looked for the
display, so the app's question at the attach is answered "VM not running" and
must be read as a display that is there. The leg wants QEMU's input layer and
all three devices to have been handed nothing, the script to have run its
three steps, and the app's lines about its input to be exactly two: the
first refusal and the second (it says each doubling). Three events are
refused, the two places and the press; a fourth refusal would be said, and
there is none because the release of a press QEMU refused is not sent.

The eighth and ninth legs, `capture` and `capture-unbound`, run the captured
pointer's script (`--scripted-capture`) on the first machine and on the one
whose tablet is bound to nothing. The script says the two moves as AppKit
read them back from the events it was handed (51,-26 and -20,10 points), the
view's points to a guest pixel, that the Mac's pointer was not held, and
that the pointer was given back when the window moved, and the leg wants
exactly that line. The scale is not the app's to say: the leg requires the
five thirds of a point to a guest pixel that a 640x480 picture has in
1280x800 points, and fails on any other. From the points and that scale it
works out the guest pixels itself, with the remainder carried, and wants
QEMU's input layer to have been handed, addressed to no console: the first
move, the left press, the second move, the left release, a notch up, the
right press and the right release, each a report of its own, an axis that
came to nothing left out. Ahead of them the script hands the view a move it
made while the pointer was free, and there must be no report of it. The
right release is sent after the pointer was given back, and over the
picture, so the view gave it a place: it must reach the mouse with none.
After them come what the script made while the pointer was captured and
handed over after the give-back: a move, of which there must be nothing,
and a left press and a notch, each addressed to console 0 with no place.
Then that press's release, a left click, and, once the script has captured
again and moved the window by a point, one more left click: each half
addressed to console 0 with the picture's middle as its place, which is
what shows that the move ended the capture. On the second machine the
place goes ahead once, of the first report that has one. Then the devices'
own reports: the mouse's are the seven, and on the second machine the later
buttons and the notch after them; the tablet's are the press and the notch
with no place and five reports with one, or on the second machine its six
places. Last the picture: the script
leaves the pointer captured, with a 3x2 image defined as a guest defines
one (hot spot 1,1) and the guest's place said to be 100,60, and the app's
picture at the end must be QEMU's screendump everywhere but there: red,
green and blue on 99..101,59, white on 99,60, the screendump's own pixel on
100,60 (the image's clear pixel, whose colour bytes are not zero), and on
101,60 the half-strength colour over half of what the screendump has
(within 2 of a channel). The leg fails if the screendump already has one of
those colours in its place, because then it would show nothing.

The tenth leg, `captured-click`, runs the script a live guest is tried with
(`--scripted-captured-click 600,108,-200,100`) on the first machine: the
pointer to a pixel as the tablet's, then captured and moved 200 guest pixels
left and 100 down, then a left click. The leg wants the script's three
lines, and from the second the points it moved and the view's scale,
which must be five thirds here too: the
points must be the whole points that many pixels are at that scale, and
the guest pixels they make (-199,100 of -333,167 points at five thirds of
a point to a pixel) are what QEMU's input layer must have been handed
after the place, addressed to no console, with the press and the release
after them. The tablet's driver must have been handed the one place, and
the mouse's the motion and the two halves of the click.

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
  (639, 240) and (320, 479), and the other three legs stay green (two pixels
  to a point; against the QEMU first on the path, which is 10.0.2: this note
  said 11.1.3 until the same day, unchecked). Through the unit tests, twenty
  changes of the rule and of the pointer's placement are each red: the rule
  never or always applied, its bound exclusive, the shorter side bounding,
  either side alone asked, the place not floored on either axis or on one, the
  pointer counted from the top, the pointer's place found in points; the
  filled place as it was before round 1's fixes, a filled side not rounded to
  pixels, the wrong side taking the area's, a flat picture no rows high; the
  pointer mapped with the width's scale both ways, each of the picture's four
  edges given to the other side, the pointer counted from the bottom. One more
  stays green and is meant to: without the clamp to the last pixel nothing
  changes, because no input was found for which the division rounds up to the
  count (a guard; the sweep that looks for one is in the unit tests).
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
- The pointer over the monitor (2026-11-12, QEMU 11.1.3 unless said). The
  app as it was (0d276a5d0, built into a directory of its own) is red on the
  legs as they now are, in two ways. On the machine with the tablet bound to
  nothing: all four positions refused, `Mouse is not absolute`, the tablet's
  driver handed nothing and the mouse's the buttons with no place. That is
  the defect, shown without a guest. On the machine with the tablet bound it
  is NOT refused, which I had predicted it would be: a console goes by the
  device bound to it whatever came up last (`ui/input.c`,
  `qemu_input_find_handler`). There it is red by where its clicks land,
  (319, 239), (9, 9), (637, 477) for (320, 240), (10, 10), (639, 479), by a
  place and its button being two reports, and by its calls on the display
  connection's pointer interface.
- Eleven changes of the new code, each built apart with a hash of its own and
  each red where expected (`aux-gate/macrunner/scale/ptr/pmut.py`). Red in
  the script: events not addressed to the display (refused, `Input handler
  not found for event type abs`, and a unit test); a button sent after its
  place and not with it (two reports, in what reached QEMU and in what the
  tablet handed its driver); a notch sent as the wheel going down and coming
  up; a notch with no place; the axis value rounded down ((9, 9) for
  (10, 10); two unit tests too); a release over the picture sent with no
  place. The unit tests do not see four of those six, and are not meant to:
  what `send_input` puts in one command is tried only against QEMU. Red in
  the unit tests alone: an event taken for the answer to a command; a
  refusal ending the input thread; a QEMU with no `gpu0` addressed all the
  same; the attach's deadline left on the monitor; QEMU gone taken for a
  refusal. The unchanged tree is green on QEMU 10.0.2 and 11.1.3 (binary
  9d23f490a479e4b0; and again, with the `click` leg and the eleven changes
  put to it once more, binary df8de09b05e5cfde).
- A phase that could never run was written and taken out again: the bar
  script set the content half a point past the scanout, to get one odd pixel
  at two pixels to a point, and AppKit made the window 641x481 points. A
  window there has an even number of pixels each way.
- The query that guards `bar-unbound` does not say what a console with a
  bound device goes by: on the bound machine it also answers that the mouse
  is current. It is asked only where the tablet is bound to nothing.
- `bar-unbound` reads each device's reports and not the order between the
  two devices. Read by hand on that machine (`-trace virtqueue_flush`, one
  command with a place and a button, 10.0.2 and 11.1.3): the mouse's two
  events are pushed before the tablet's three. On the bound machine the same
  command is four events on the tablet's queue and one notification.
- The `click` leg turns red with the axis value rounded down: the middle
  arrives as 319,239 and the click as 599,107, in QEMU's input layer and on
  the tablet alike (the last pixel still as itself: its axis value is the
  axis's last, whichever way it is rounded). Nothing else was put to it; the
  leg is there to hold the script to what it says, not to find a defect the
  other legs do not.
- Audit round 2 on the pointer (2026-11-12) found by reading what no leg
  had shown: a release QEMU refuses for a stopped machine was dropped. The
  legs ran no stopped machine and the unit tests had no monitor that refuses
  and then takes. Both exist now, and its other findings changed two legs
  (the tablet has the launcher's name; `bar-unbound` wants the place ahead).
- Twenty-two changes of the code as it is after that round, each built apart
  and each as expected (`pmut.py`, `pmut-run3.txt`; QEMU 11.1.3). The eleven
  above again, three of which the unit tests now see as well (a button after
  its place and the two about a notch), because
  what goes into one command is now made where a scripted monitor can read
  it. Eleven new. Red in a leg and in the unit tests: no place ahead of a
  button on the unbound machine, a place ahead even where the guest has it,
  a tablet bound to nothing taken for a bound one (all `bar-unbound`); the
  release of a refused press sent (`stopped`: a fourth refusal is said); a
  stopped machine's display taken for none (`stopped`). Red in a leg alone:
  the place QEMU gives for a pointer not printed, and the script aiming a
  pixel to the right (`click`: it says `meant 320,240 of 640x480, sent
  321,240`, and of the last pixel `sent nothing`). Red in the unit tests
  alone: a refused release not sent again; a move ahead of a refused
  release; the release not tried before a key or at a Sync; a tablet that
  is not found taken for a bound one. The unchanged tree is green on QEMU
  10.0.2 and 11.1.3 (binary ef213fced141ba30, 27 s a run).
- The first run of those controls stalled, and that was a defect of a test:
  with the release not tried at a Sync, a new unit test waited for ever on a
  scripted monitor that was reading a command nobody would send. The
  scripted monitor now gives up after ten seconds, and the same change is
  red.
- The captured pointer (2026-11-12). The two capture legs passed the first
  time they ran, so what shows that they can fail is their controls:
  eighteen breakages of the change, each built in a copy of the tree and put
  to the unit tests and to the leg that should see it, each with the verdict
  expected (`pmut.py`, `aux-gate/macrunner/scale/rel/cmut1.out`; QEMU
  11.1.3), and each red read for its own message. Red in a unit test and in
  `capture`: the mouse's commands addressed to the screen (the trace has
  console 0 where it must have none); a button coming up on the device the
  pointer is on by then (the right release arrives with a place); the rest
  of a pixel dropped (the second move arrives as -12,6); an axis that did
  not move sent (every button's report has two zero motions in it); a
  captured notch, and a captured press, handed to the tablet; the pointer
  drawn by its corner; a colour stronger than its alpha drawn (the clear
  pixel shows 10,20,30); the pointer's alpha applied to its colour again
  (the half pixel is 50,25,10). Red in `capture` alone, in code only a
  window reaches: a captured press counted only with a place (no button
  arrives, and the two moves arrive as one, 19,-10, which is their sum);
  the input thread not told of the capture; the pointer drawn where it was
  first and not where the guest says. Red in `map` alone: the pointer drawn
  when it is not captured (the picture is no longer QEMU's screendump). Red
  in a unit test alone, where no leg has the case: of a run of motion only
  the last sent; a button overtaking the motion before it (the script sends
  faster than the input thread takes, so a leg could go either way); the
  place the guest was last sent still standing after motion; the pointer
  drawn over the bars (the picture is read back at 1:1, which has none);
  the place the guest says not kept (the script says the place itself).
  The twenty-two older controls of the pointer were run on that tree too,
  forty in all, each with the verdict expected
  (`aux-gate/macrunner/scale/rel/cmut-all.out`). Three of them had to be
  pointed at the reworked code: the two of the notch, and the release of a
  refused press, which now needs two changes, because the release is kept
  back in two places. The paced script's leg has two controls of its own, each red in `captured-click` alone: the script moving a point more than it means (the leg says the points are not the pixels asked for), and the script clicking without capturing (the click arrives on the tablet, with a place). An older control whose anchor the new script made ambiguous was pointed at the click script again and is red as before.
  Not shown by any control: the order of the display link's two steps (the
  pointer's image is taken up, then the frame drawn; the other order is one
  frame late and nothing else); the remainder dropped at a capture (the
  script makes no move after its second capture). The unchanged tree is
  green on QEMU 10.0.2 and 11.1.3 (binary e9722ef1a0ee707f, 34 s a run, 66
  unit tests), and with the tenth leg on both again (binary 494b2da7c5b97121).
- Audit round 3 (2026-11-12). The round found three breakages of the
  owed-release and motion code that no test saw, and they are tests now.
  Its fixes have controls: seven new ones, and an older one pointed at the
  reworked line, with all forty-nine run again on the tree that has them
  (`pmut.py`, `aux-gate/macrunner/scale/rel/r3mut-all.out`; QEMU 11.1.3),
  each with the verdict expected and each new red read for its own message.
  Red in a unit test alone, each in the test written for it: a notch going
  ahead of a release that is owed, and of two releases owed the newer
  thrown away (both by the scripted monitor giving up after ten seconds,
  which is a command that did not come); a run of motion swallowing the
  capture behind it (the press after it is sent to no display and with no
  place, as the mouse's). Red in `capture` alone, in code only a window
  reaches: a move made before a change of hands taken (a report of 4,4
  ahead of the mouse's others, and `at 320,240` on the tablet after them);
  a press made before the give-back sent with its place (`at 320,240 press
  left`); a notch made before it sent with a place, which is wherever the
  Mac's pointer was over the window (`at 257,52 wheel-up` in this run; off
  the picture the notch would not have been sent, and the leg is red by
  that as well); the window's move not ending the capture (the script says
  `given back when the window moved: false`). The legs with the new cases
  passed the first time they ran, as the first capture legs had.
  Not shown by any control: the other four window events that end a
  capture (each is the same one call, and no script changes the window's
  size or screen or goes to full screen); and nothing of the Mac's own side
  under a key window, as before. The unchanged tree is green on QEMU 10.0.2
  and 11.1.3 (binary 4a9b0d13f8f3ff33, 70 unit tests;
  `headless-r3c-q11.out` and `headless-r3d-q10.out` beside the controls'
  output).
