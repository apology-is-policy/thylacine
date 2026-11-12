# Handoff 050 -- a host that resizes the display: what was changed in Lictor while you were away (to Astra)

**From**: aux, 2026-11-11. **To**: Astra, for when you are back.

This file used to ask you for a change (its first form is in git: `git log --follow` on this
path). The operator then wrote, on 2026-11-11: "Astra is on vacation until further notice.
Feel free to change Lictor." So the change is made, by aux, and this is the record of it. Your
worktree and anything you had not committed were not touched. Everything below is yours to
change or take out; the reasons are here so that you can judge them without me.

## Why it could not wait

Two defects, both reachable from the Mac app (`macrunner/app`) and from any QEMU window:

1. **A boot with no session.** With the device made 3456x2160 (full screen on a 16-inch
   MacBook Pro) the compositor adopted that size, its first full-screen surface could not
   be allocated (three buffers of it are 85 MiB against the kernel's 64 MiB a weave), and
   the display came up with a screen and no session (measured 2026-11-11,
   `docs/MAC-RUNNER.md` M-8). The `mode` verb has refused such a size since 2026-07-23; the
   boot had no rule for one.
2. **A boot with no Lictor.** `Screen::new` refused a host under 800x720 pixels or over
   4096x2160 in all, and the probe failed with it. Lictor is the keyboard and the pointer
   too. (Read from the code.)

And the feature the Mac app needs: a guest that takes a new mode when the host's window is
resized or moved to a display with another scale.

## What changed in Lictor

- **`backend/gpu.rs`: the device's events are taken and the display ones counted.**
  `Controlq::take_events` reads `events_read` (device configuration, offset 0); if it is
  not zero it writes the same bits to `events_clear` (offset 4), and if bit 0
  (`VIRTIO_GPU_EVENT_DISPLAY`) was among them it adds one to a wrapping `display_events`.
  `service_completions` calls it when a configuration change may have happened: under
  INTx when the ISR byte, which it already read and threw away, has bit 1; under MSI-X on
  every interrupt ticket, because `select_virtio_vectors` gives the configuration
  interrupt the queues' one vector and nothing tells the two apart. That is one register
  read more for each ticket. At start, just before `read_display_info`, pending events are
  taken and the count set to 0: the first question supersedes them.
- **`gpu_api.rs`: `Stats` carries the count.** `display_events: u32`, between
  `condemned_lost` and `engine`. `Stats` is on every reply, so the compositor learns of an
  event with whatever it asks next, and it asks something on every pass. The record's
  layout changed. It is decoded in one place, the client in `rpc_client.rs` that every
  user of the broker links.
- **`proxy/gpu.rs`**: `Gpu::display_events()` reads the count from the last reply the
  client had, of any kind. It is not a field of the proxy, because `query` does not
  refresh those and the count would have missed a reply (audit round 1, F2).
- **`backend/gpu.rs`: `query_edid` tells no EDID from no answer.** It answered `Ok(None)`
  for every way GET_EDID could fail. Now `Ok(None)` is the device answering with something
  other than an EDID (or a block that is not one), and an error is the command not
  answered: the engine takes no commands, or the deadline passed. It matches on
  `submit_and_wait` itself in place of `step`, and says the same two lines `step` would.
  Your start still reads an error as no EDID. The compositor needed the difference: a
  stall at that step set its scale to 100 and it called the follow done (audit round 2).
- **`render.rs` and `backend/screen.rs`: the trusted screen has a size for any host.**
  `render::fits` is the old test (800x720 at least, 4096x2160 pixels at most), now one
  function that `render()` asks too. `Screen::size_for` gives the host's size if it fits,
  else that size over or times a whole number up to 4, the first that fits
  (`libhalcyon::display::fit_mode`), else 1024x768. The screen is still made once,
  at start; it says so when its size is not the host's (`lictor: trusted screen WxH for a
  display of WxH`). A side is also at most 8192. Nothing in the device scales it: QEMU
  makes the console's surface the scanout's own size, and the frontend decides how to
  show it (an earlier form of this file said the device does; audit round 1, F3).

## What changed outside Lictor

- **`usr/lib/libhalcyon/src/display.rs` (new), pure and host-tested.** `fit_mode(host_w,
  host_h, can)`: the host's size if `can` accepts it, else over 2, times 2, over 3, times 3,
  over 4, times 4. A whole factor keeps the host's picture of the screen sharp, and the
  scale needs nothing more: the same millimetres under half the pixels are half the DPI.
  `Follow`: the timing of a follow as a small state machine over the count and a clock
  (150 ms of quiet, once; a pinned mode stands; an event whose asking finds the device
  stalled is held, uncounted, until the device answers or is dead; an asking that was made
  and failed, a stall that began inside it included, is made again, at most six times over
  18.9 s). `Backoff`: when to try again something that stays owed, at waits doubling from
  300 ms to 9.6 s.
- **`usr/tapestryd/src/server.rs`**: the first mode at boot, `mode auto` and a followed
  display event all take the fitted mode (`mode_in_bounds`, and `mode_can_be_made`: a
  probe allocation of the screen and one full-screen surface, held together, are the
  `can`). `display_pump`, called once a pass after the fence pump, hands the count to
  `Follow` and, when it says so, does what `mode auto` does. It reads your broker's
  engine state first: a stalled engine is not asked and the event waits for it; a dead
  one ends the matter. An explicit `mode W H` is judged by the same test and pins the
  mode until a `mode auto` that is accepted. After a scanout command that your engine did
  not answer, the compositor does not know what the device scans (the proxy's
  `scanout_unsettled`, below), and `bind_pump` settles it once a pass while your engine
  is live. `docs/AURORA-CONFIG.md` 3.4 has the rule, and the compositor's dossier the
  whole of it.

## The five questions this file put to you, as they were answered

1. **When to follow.** From boot, and after `mode auto`. Not after an explicit `mode W H`:
   a person who set a size meant it. A persisted fixed mode is pushed as `mode W H` at
   startup, so it is pinned too.
2. **Trains.** The count is followed once it has not moved for 150 ms; a train is one mode
   change. The Mac app itself waits 250 ms after a resize ends, QEMU's own windows a
   second.
3. **The trusted path.** No mode changes while the trusted screen is up: the compositor's
   pass returns at the seat's phase before it reaches the pump, and the count that arrived
   with that reply is followed after the screen comes down. The trusted screen itself keeps
   the size it was made at.
4. **Tearing.** The switch is the existing mode change (`change_mode`, what `mode W H` has
   always done): a new screen is bound before the old one is freed. Nothing new was
   argued for I-40. Its order changed in one way: the new screen is allocated before the
   menu's card is dismissed and held presents are dropped, so a change that cannot be
   made touches nothing.
5. **A zero size.** `query_display_info` answers None for a disabled scanout and the mode
   stands (`no display to follow; WxH stands`). A question the device does not answer is
   a different thing and is asked again.

- **`tools/interactive/ls-display-follow.exp`** (new) boots a device of 3456x2160 and then
  resizes it as a VNC viewer does (`rfb_resize.py`, SetDesktopSize): no Mac app needed.

## What I would want your eye on

- The trusted screen knows no display scale. Its layout is in pixels, so on a display the
  session runs at 200% (the Mac app's default window) its text is half the session's
  physical size, and at a trusted screen of 3456x2160 beside a session of 1728x1080 the
  panel is under a fifth of the width. Two ways I can see: bake the faces at 2x as well and
  pick by the EDID, or make the trusted screen at the session's mode and let the frontend
  scale it. Not done; nobody has looked at the trusted screen under the Mac app yet.
- The trusted screen is made once. After a followed resize it is the boot's size, shown
  as the frontend shows a scanout of another size. Following there too would mean a new
  `Screen` between episodes.
- A bind that meets a stalled engine. tapestryd's `reconcile()` enters its composed state
  whether or not the device took the bind, and a mode change takes its own bind back when
  it is not accepted, though the device may execute it later and scan a screen that was
  given up. **Your proxy now carries a flag, `Gpu::scanout_unsettled` (`proxy/gpu.rs`).**
  Each of the four scanout commands notes the stall count of the client's last reply
  before it calls; accepted, it clears the flag; not accepted with the count moved, it
  sets it; a refusal that was answered leaves it. It is your `condemn_if_stalled` test,
  read from the client's side, and it errs to "not known" when another client's deadline
  passed between two of this client's replies (no other client sends your engine work, so
  that is a command of your own: the trusted screen's, or a test lever's). The compositor
  decides whether a bind is needed by `bound_res` only while the flag is clear, binds its
  screen again from its pass while your engine is live (`bind_pump`), and then, where the
  display stays composed, repaints and pushes everything again; where one client fills
  the display it only begins the switch to that client again. Seven other readers of
  `bound_res` in the compositor decide what may be let go, with or without the flag, and
  what makes them safe is your condemn and not the field. A flag kept in the
  compositor and set by hand at each bind came first, and round 3 found a bind it had
  missed, which is why it moved to where every scanout command passes. It rests on two
  things of your engine that I read in your code and would want you to confirm: the
  device takes the control queue in order, so a bind whose deadline passed is executed
  before any later command or never; and an accepted SET_SCANOUT lets go of what was
  parked before it, all but what it bound and any entry whose deferred free your engine
  does not take (`drain_condemned`; the blob probe drains nothing).
- **A thing you may want to do instead.** The broker knows what it last accepted
  (`presentation`). If it bound that again itself when a stall ends, the device would be
  back at what every client believes without any client's help, and a client would need
  to know only that a stall happened, to push its content again. I did not do it: it
  changes the stall design (`docs/GPU-DESIGN.md` 4.5.15), which was voted, and the
  proxy's flag needs no change to your engine.
- **Your stall lever has three more shots** (`TestArmLatch` gives six `TestStallNext`
  now, `backend/device.rs`). The compositor aims the new three at binds of the display's,
  chosen by the height of the host a display event tells, and
  `tools/interactive/ls-display-stall.exp` is the scenario for them; what it has shown
  in a guest is under Measured. A request you refuse by your own checks before a command
  is staged (a scanout command naming a resource that is not an ordinary one) leaves a
  shot armed for the next synchronous command; the compositor aims at no such bind, and
  I left that as it is. (An earlier version of this file said nothing here could make
  your engine stall on a bind. That was wrong: your lever could, and I had not looked.)
- One read of `events_read` for each ticket under MSI-X. A vector of its own for the
  configuration interrupt would remove it; `PciIrq::for_virtio` gives one vector to all.
- The INTx branch (the ISR bit) is in no gate.
- An event raised between the read and the clear is cleared uncounted. It is covered:
  the count made by that call is followed by a question asked after both.

## The audit

Round 1 (a reviewer of another model family, with nothing booted): no P0, no P1, one P2,
six P3. The P2 was that an event could be lost to a stall on three paths the first retry
did not cover; the hold above is its fix. The P3s are the corrections marked above, the
order in `change_mode`, a `mode auto` that unpinned before it was accepted, and the
regression scenario's waits.

Round 2, on those fixes: no P0, one P1, two P2, five P3. The P1: a mode change whose bind
your engine did not answer took the change back, and the device could then execute the
bind and scan the given-up screen, black, with nothing to bind the standing one again;
the flag and `bind_pump` above are its fix, and they also close the open defect this file
named before (an explicit `mode W H` left composed and not bound). The P2s: `query_edid`
as above, and an event held without a count through a stall that began inside each
asking, which would have had the change begun again without end and a screen parked each
time; an asking that was made is now always counted, and no screen is made while a bind
is owed. The P3s: held presents were dropped before the bind that could still fail; a
follow that changed the mode and then failed was reported as not followed (it is
`Unfinished` now, and `mode auto` answers Ok for it); three changes to the timing that no
host test caught; the scenario could pass a failure said just after its success line; and
fifteen sentences that said more than the code does.

Round 3, on those fixes: no P0, no P1, one P2, four P3. The P2: the flag round 2 added
was set by hand where a screen is bound, and the switch to direct binds a client's buffer
while the scanout still reads composed; that bind unanswered, and the layout then wanting
the composed state, left the device scanning the client's buffer with nothing looking
again. The flag is now the proxy's, as above. The P3s: the late bind pushed nothing, so
the glass kept what was last pushed before the stall (everything is established again
now); the scenario saw only changes begun from the direct state (a second scenario,
`ls-display-stall`, begins one from the composed state and stalls three binds); a scale
reset, `scale auto` or `mode W H` between a change and its display's EDID used the old
display's millimetres (the scale now stands until the EDID is read); and eight more
sentences.

Round 4, narrow, on the proxy's flag and those fixes: no P0, no P1, no P2, five P3. It
found no way for the flag to miss a command your engine may still execute while the
engine lives, and it re-derived the lines the stall scenario waits for. The P3s: a
retired surface's GL frame can stay on the glass in the off state (a re-target, then a
close before the next present; older than this work, open, and on a path that needs a
GL host, so not changed here); nine more sentences; no red control took the proxy's flag
away (one does now); a pinned mode kept the millimetres of the display that was, and a
scale reset before a new display's EDID was read kept an override's value (the EDID is
now asked for again whenever a scale is derived from a stale one, and a pinned event
marks it stale); and a stall shot could stay armed for a later display's bind (the next
aim and a failed change now clear it).

## Measured

Part 1, on the commit before round 2's fixes (QEMU 11.1.3, HVF, an M2 Air). On the image
from before this work a device of 640x400 and one of 5120x2880 each came up with no
compositor (your probe failed four times), and a size told to the running guest was not
followed. With this work: 640x400 gives your trusted screen 1280x800 and a session at
640x400; 3456x2160 a session at 1728x1080; 5120x2880 your trusted screen and the session
at 2560x1440. The Mac app's window was followed twice (1280x800 to 2560x1600 at 200%,
then to 2000x1280), and a size told over the wire was on the scanout 214 ms later.

Part 2, on the commit that carries round 3's fixes (4dd462eb8). The bare image as in part
1, the three sizes and the Mac app's window; the app's frame after the followed resize is
QEMU's dump byte for byte at 2000x1280, and a size told over the wire was on the scanout
282 ms later (one sample, as the 214 was). The same resize with the GPU's driver on the
interrupt line (a local change that makes `auto` the line; `gpu PCI IRQ mode=Intx
vector=65535`) gives the same bytes: the INTx branch has run that once and is in no gate.
On the gate image `ls-gfx-mode` passes (9) and `ls-display-follow` passes (7) in its
first run in a guest; with the first mode left unfitted it is red at the boot, and with
the display pump not called it is red at the first follow after a green boot.
`ls-display-stall`'s first run in a guest said every line of its first leg in the order
derived, your lever's two among them (`retired past its deadline (INJECTED stall shot of
test-latch-dead) -- engine stalled`, then `retired 0 ms after its deadline -- engine
revived`), then `BIND FAILED`, `bound, late` and `followed`; and then it failed at reading
the picture, for a reason that is neither yours nor the compositor's: QEMU writes each
row of a PPM rounded up to four bytes, so a width that is no multiple of four (1118)
gives a file longer than its header says. `tools/screendump.sh` now hands out a true P6.

Part 3, on the commit that carries round 4's fixes (bfda0ba4c), 2026-11-12.
`ls-display-stall` passes whole in a guest (7 passes): each of the three binds your lever
stalls is put right, in the order derived. Entering the composed state: `BIND FAILED`,
`the display is not bound to its screen`, `bound, late`, then `followed`. The switch to
direct: `bind REFUSED`, then `bound, late`, then the switch begun and made again. The
change's own bind, from the composed state: `not followed (errno 5), to be asked again`,
then `bound, late` for the standing screen, and only then the second asking, which makes
the change. Your engine said the same two lines at each (`engine stalled`, then `engine
revived` 0 ms after the deadline). Two red controls, each an image of its own: with the
compositor's pass not calling `bind_pump`, and with the proxy's flag never set, the
scenario passes the boot and the first leg and is red at the second, where a later present
makes the switch with nothing having bound the screen. On the same clean image
`ls-display-follow` passes (7) and `ls-gfx-mode` passes (9). The bare image gives a session
at 1728x1080 for a device of 3456x2160 and follows the Mac app's window, byte for byte
again; the kernel's suite on it passes (2447 tests, none failed).
