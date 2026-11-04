---
id: chg-2026-11-04-compositor-band
type: chg
title: "The designated compositor's threads are INTERACTIVE"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-sched
established:
  - dec-2026-11-04-compositor-band
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
The compositor warden designates runs in INTERACTIVE, a third leg beside the IRQ and trusted-console ones. `proc_set_seat_client` leaves `PROC_FLAG_SEAT_CLIENT` when it takes, `apply_spawn_perms` marks the child's first thread on that result, and `thread_new` gives later threads the band from the flag. `scheduler.seat_client_takes_interactive` runs the mapping on a real child's own thread, with a refused designation and a fork as its negatives. What the real boot's tapestryd holds is checked by measurement, not yet by a durable scenario, since `/proc/<pid>/sched` is the owner's or a hostowner's.
