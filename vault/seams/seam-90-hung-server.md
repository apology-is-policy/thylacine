---
id: seam-90-hung-server
type: seam
title: "Dying-reader liveness against a hung/untrusted server (mid-frame stall)"
status: open
surface: [sub-kernel-ninep-client]
opened-by: chg-2026-07-19-90-death-block-through
tracker: "v1.x"
created: 2026-07-31
updated: 2026-10-06
---
## Owed

The #90 block-through makes a dying elected reader finish its current frame
before unwinding — bounded by the TRUSTED server's whole-frame delivery. An
untrusted or hung server that sends a partial frame then stalls hangs the
dying reader mid-frame indefinitely (the #845-F1 untrusted-server class,
liveness face).

## What closes it

A bounded mid-frame wait (per-transport deadline for untrusted transports,
or a frame-progress watchdog) — designed together with the untrusted-server
work ([[seam-845-untrusted-server]]), since both gate on the same trust
boundary.

## Risk while open

None at v1.0: every 9P server is a trusted local Proc and a hung server
already hangs LIVE readers regardless — #90 introduced no new hang class,
it converted a reachable corruption into this bounded v1.x liveness debt
(strictly better).

## As of 2026-10-06

- The risk paragraph above is WRONG: `SYS_ATTACH_9P` takes pipes from any
  process (no capability gate), so an unprivileged server can send part of a
  frame and stop, holding a dying or stopped reader of its mount. The hold
  ends when the server dies (every holder of the pipe's write end), so it is
  bounded by the server's own life, not by trust.
- The partial frame is now the client's (`c->rx_got`,
  [[chg-2026-10-06-loom-multiclient]]): an unwind mid-frame no longer
  desyncs the stream. The close no longer needs a deadline or a watchdog --
  let a death or stop unwind at any byte -- but it changes ARCH 8.8.1.1's
  voted design (block-through), so it is the operator's decision (owned:
  OPEN-BUGS 2026-10-06). The pumps (Loom waiters, the dev9p poll kthread)
  never wait inside a frame, so only blocking sync readers carry the seam.
