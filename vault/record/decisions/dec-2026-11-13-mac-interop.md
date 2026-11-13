---
id: dec-2026-11-13-mac-interop
type: dec
title: "The Mac's 9P tree: a channel per session through /srv/mac, version 1 is snarf and folders, and Quit presses the power button"
date: 2026-11-13
status: standing
decided-by: user-vote
affects: []
created: 2026-11-13
---
## Fork

Vote M5 put the Mac's interop on one 9P tree served by the app over a virtio-serial port and
mounted at `/mnt/mac` ([[dec-2026-11-10-mac-runner]]). The interop station had to fix what
`docs/MAC-RUNNER.md` 4.4 left as a draft before either side wrote a byte. That meant how the
guest's sessions reach the one port, what version 1 of the tree holds, and how the app's Quit
asks for a shutdown. The guest's facts shaped the first question. The kernel's 9P client is
9P2000.L only, it makes one attach per connection, and a pipe-pair attach is capped at 4 KiB
messages where a `/srv` connection gets 32 or 128 KiB. A dead server leaves its mounts reading
EIO in every program that copied the namespace.

## Decision

The operator, 2026-11-13, each as recommended.

- **M10:** channels. The new virtio-serial driver frames the port into one channel per
  connection and posts `/srv/mac`. login mounts it at `/mnt/mac` in each session. Each session
  is its own 9P2000.L session with the app, attached as that user, and the app shows every
  file as owned by the attaching user.
- **M11:** version 1 of the tree is `snarf` (drawterm's `/dev/snarf`: a read from offset 0
  takes the pasteboard's text, a write replaces it at close) and `folders/<name>/`.
  `display/` is dropped, and `power`, `time` and `open` come later as additions.
- **M12:** Quit presses the `virt` machine's power button through QMP `system_powerdown`. A
  new guest power-key driver turns the press into an orderly shutdown.

The same batch settled audit R4-F3: Lictor counts the restores that end blank in its Stats,
and the compositor rebinds when the count moves.

## Why

A channel per connection keeps the driver a byte relay that parses no 9P. Each session gets
its own identity and `/srv`'s message size, which haul's one pipe mount could not give. It
answers the guest's one-attach-per-connection client without a 9P multiplexer in a driver.
The two files are what the alpha needs, and adding files later is no break; virtio-gpu already
tells the guest its display. The power button is how VM managers ask a guest to stop. It keeps
power control out of a per-session tree and works without the interop line.

## What it costs

A framing protocol that is ABI between the app and the driver (MAC-RUNNER 4.4, version
`mac1`). Two new guest drivers, each an audit surface. A Mac-side restart leaves running
mounts reading EIO until a remount or the next login.
