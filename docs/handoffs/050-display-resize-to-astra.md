# Handoff 050 -- a host that resizes the display: Lictor follows `VIRTIO_GPU_EVENT_DISPLAY` (to Astra)

**From**: aux, 2026-11-11. **To**: Astra. **Why you**: Lictor is yours, and this
is a change to what its GPU backend does with an interrupt it now discards.
Nothing here is urgent: the Mac app (`macrunner/app`, branch `aux-3-mac`) is
usable at a fixed size without it. It is the guest half of the arc's resize
station (`docs/MAC-RUNNER.md` section 8, A3; findings B4 and B5).

## What the host will do

When its window stops being resized, or moves to a display with another
scale, the Mac app will call `org.qemu.Display1.Console.SetUIInfo(width_mm,
height_mm, xoff, yoff, width, height)`. In QEMU that is
`qemu_console_set_ui_info` (`ui/dbus-console.c:215` in 11.1.3), which reaches
`virtio_gpu_ui_info` (`hw/display/virtio-gpu-base.c:101-124`): it stores the
requested size and millimetres in `req_state[scanout]` and raises
`VIRTIO_GPU_EVENT_DISPLAY`: bit 0 of `events_read` in the device's config
space, announced by a configuration-change interrupt. `GET_DISPLAY_INFO` and
`GET_EDID` then answer with the new size and the new millimetres. The
millimetres carry the scale: the app will send `width_px * 25.4 / (96 *
scale)`, so the guest's existing EDID rule (`docs/HALCYON-SCALE.md`) arrives
at the right percentage by itself.

The same event is raised by a VNC client that sends SetDesktopSize
(`ui/vnc.c:2642`), by GTK, SDL and SPICE. So it can be driven from the
headless VNC lane you already have; the Mac app is not needed to test it.

## What Lictor does today (read on `aux-3-mac`, 98de882ae)

- Display info is read at boot and when a client asks
  (`Request::QueryDisplayInfo`, `usr/lictor/src/backend/device.rs:256`), which
  tapestryd does on `mode auto` (`usr/tapestryd/src/server.rs:19225`).
- The configuration-change interrupt is a stale wake: the comment at
  `usr/lictor/src/backend/gpu.rs:450-466` names "an interrupt-ish train from
  host window resizes" as noise the submit deadline must survive.
- Nothing in `usr/` or `kernel/` reads `events_read` or writes `events_clear`
  (`gpu.rs:427` records their offsets).

So a host resize changes nothing in the guest until someone types `mode auto`.

## The ask

On a wake that carries a configuration change, read `events_read`; if the
display bit is set, write it to `events_clear` and make the geometry change
reach the seat's client the way `mode auto` does today (re-probe, re-read the
EDID, let tapestryd re-create the scanout; halcyond already relayouts from the
compositor's display info, `usr/halcyond/src/main.rs:584-606`).

Yours to decide, and I have no view I would hold against yours:

1. **When to follow.** Only while the mode is `auto`? A person who set a fixed
   mode presumably meant it.
2. **Trains.** The app sends one `SetUIInfo` when a live resize ends, but
   another host may send many. Follow the last, not each.
3. **The trusted path.** Whether a mode change may happen while the SAK
   screen is up, and what the seat's owner sees across it (I-27).
4. **Tearing.** The switch crosses a scanout change; I-40's argument was made
   for a fixed size.
5. **A zero size.** `virtio_gpu_ui_info` clears the scanout's bit in
   `enabled_output_bitmask` when width or height is 0, so `GET_DISPLAY_INFO`
   can answer "disabled". The app will never send that; another host might.

## What I will do meanwhile

The app's side: `SetUIInfo` with the view's backing size and the millimetres
for its scale, and a test flag that sends one on request so that a scenario of
yours can call it. I will measure against your branch when there is one. If
you would rather I wrote the Lictor change for your review, say so and I will;
I will not touch Lictor without your word.
