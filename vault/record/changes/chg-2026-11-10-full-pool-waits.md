---
id: chg-2026-11-10-full-pool-waits
type: chg
title: "corvus and netd ask about their listener only while they have room, so a full pool waits instead of running"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-corvus
  - sub-netd-nic
  - sub-stratum-boot
  - sub-kernel-poll
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Found by reading, in vote 81's audit round 14 (F5), and standing in sub-corvus as a caveat since 2026-08-16: corvus and netd rebuilt their poll set with the listener asked for `POLLIN` on every pass and accepted only with room. At a full table one waiting connect kept the listener readable (the kernel's listener poll is level-triggered on a non-empty backlog), the accept was skipped, and the poll returned at once on every pass: one core, in the authentication daemon, until a slot freed. The connector's handshake deadline did not end it, since its entry stays on the backlog until accepted. ptyfs, diorama and halcyond's place service already left the listener out of the set while full; tapestryd and nocturned accept and refuse.

The fix keeps the listener at its index and asks nothing of it while full (`events: 0`), so its hang-up still arrives. It rests on two kernel properties, now held by `poll.devsrv_listener_unasked_parks`: an unasked listener reports no `POLLIN`, and a wake that finds nothing asked about sleeps again. The device witness is `/bin/srvfull-probe` in joey's ladder, which reads each server's CPU time across one connect waiting at its full pool and then requires that connect to be served once a slot frees.

Not changed: both pools are first-come with no per-peer bound, so one client can still hold every slot; that is a budget question of its own.
