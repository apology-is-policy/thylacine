---
id: chg-2026-11-05-seat-takeover-stall
type: chg
title: "A takeover that meets a stall starts over; no wide wait in an episode; the cursor lane comes back"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
Vote 50's audit, round 1, read that a client's backlog still denied the SAK where its poison no longer did: the trusted paint is fenced, so it stalls behind what the host has queued, and a stall failed the episode at once. Now a takeover that meets a stall starts over when the engine revives, under the kernel's deadline (F1). No wait in an episode takes the abandonment bound, so the paint cannot stop the serve loop for 30 s behind a readback (F3). A serving `cursor_command` stalls at 500 ms and never widens, and a chain past 30 s is abandoned rather than poisoned, so a well-formed late retirement frees the lane and one bad stretch no longer denies the SAK until reboot (F2). tapestryd retries a refused pointer update instead of latching it (F7). A malformed cursor entry still poisons until device reset. A backlog longer than five seconds, or a host loop held that long, still fails the episode with nothing on the glass; [[dec-2026-11-03-scoped-retirement-and-friends]] did not settle that and an operator vote is owed. `ls-graphical-sak-dead` is re-specified: leg S takes the retried takeover, leg 1 holds the stall past the kernel's deadline. `sak_glass.py` reads the trusted ground off a capture in the chain and poison scenarios. Not booted when committed.
