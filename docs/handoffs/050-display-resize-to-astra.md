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
  layout changed; everything that decodes it (tapestryd, corvus, caps-probe) is built from
  the same tree.
- **`proxy/gpu.rs`**: the proxy keeps the last count it was told.
- **`render.rs` and `backend/screen.rs`: the trusted screen has a size for any host.**
  `render::fits` is the old test (800x720 at least, 4096x2160 pixels at most), now one
  function that `render()` asks too. `Screen::size_for` gives the host's size if it fits,
  else that size over or times a whole number up to 4, the first that fits
  (`libhalcyon::display::fit_mode`), else 1024x768. The screen is still made once,
  at start; it says so when its size is not the host's (`lictor: trusted screen WxH for a
  display of WxH`). The device scales it to the host's rect.

## What changed outside Lictor

- **`usr/lib/libhalcyon/src/display.rs` (new), pure and host-tested.** `fit_mode(host_w,
  host_h, can)`: the host's size if `can` accepts it, else over 2, times 2, over 3, times 3,
  over 4, times 4. A whole factor keeps the host's picture of the screen sharp, and the
  scale needs nothing more: the same millimetres under half the pixels are half the DPI.
  `Follow`: the timing of a follow as a small state machine over the count and a clock
  (150 ms of quiet, once; a pinned mode stands; an asking the device did not answer is
  made again, at most six times over 18.9 s).
- **`usr/tapestryd/src/server.rs`**: the first mode at boot, `mode auto` and a followed
  display event all take the fitted mode (`mode_in_bounds` and a probe allocation of the
  full-screen surface are the `can`). `display_pump`, called once a pass after the fence
  pump, hands the count to `Follow` and, when it says so, does what `mode auto` does. An explicit `mode W H` is judged as before and pins the mode until `mode
  auto`. `docs/AURORA-CONFIG.md` 3.4 has the rule.

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
   argued for I-40.
5. **A zero size.** `query_display_info` answers None for a disabled scanout and the mode
   stands (`not followed`).

- **`tools/interactive/ls-display-follow.exp`** (new) boots a device of 3456x2160 and then
  resizes it as a VNC viewer does (`rfb_resize.py`, SetDesktopSize): no Mac app needed.

## What I would want your eye on

- The trusted screen knows no display scale. Its layout is in pixels, so on a display the
  session runs at 200% (the Mac app's default window) its text is half the session's
  physical size, and at a trusted screen of 3456x2160 beside a session of 1728x1080 the
  panel is under a fifth of the width. Two ways I can see: bake the faces at 2x as well and
  pick by the EDID, or make the trusted screen at the session's mode and let the device
  scale it. Not done; nobody has looked at the trusted screen under the Mac app yet.
- The trusted screen is made once. After a followed resize it is the boot's size, scaled
  by the device. Following there too would mean a new `Screen` between episodes.
- One read of `events_read` for each ticket under MSI-X. A vector of its own for the
  configuration interrupt would remove it; `PciIrq::for_virtio` gives one vector to all.
- The INTx branch (the ISR bit) is in no gate.
- An event raised between the read and the clear is cleared uncounted. It is covered:
  the count made by that call is followed by a question asked after both.

## Measured

Nothing yet: the boots follow this commit.
