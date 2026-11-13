---
id: chg-2026-11-09-narrow-sync-wait
type: chg
title: "Lictor's synchronous wait no longer widens behind a readback (vote 72, first half)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-gallery
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Operator vote 72's first half ([[dec-2026-11-05-narrow-wait-and-key-thread]]). `Controlq::wait_sync_done` takes `SUBMIT_DEADLINE_MS` whenever the serve loop runs; `wait_widens`, `readback_in_flight` and the seat flag that only they read (`Gpu::seat_bound`) are gone. A command behind a readback now stalls the engine, which revives when the chain retires. gallery retries a refused present every 100 ms for 31 s; tapestry-demo retries its first present the same way and counts a refused frame as dropped, failing only after 31 s of refusals. warp-prove's C6-RB DEADLINE arm counts presents refused during the stall and requires a present to go through within a second of the readback retiring, with nothing poisoned. GPU-DESIGN 4.5.15's list and TRUSTED-PATH's backlog paragraphs follow; vote 71's record ([[dec-2026-11-05-sak-waits-on-backlog]]) is written, its build is next. The input thread is the second half.
