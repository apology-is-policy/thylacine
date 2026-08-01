---
id: chg-2026-07-20-g7a-sdl-seam
type: chg
title: "G-7a: nanosleep onto torpor (the SDL seam)"
date: 2026-07-20
arc: arc-tapestry
commits: ["632961ed"]
touched:
  - sub-pouch-thread
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
`__clock_nanosleep` -- which `nanosleep`, `usleep`, and
`clock_nanosleep` all route through -- rewritten from the Linux
sentinel cascade onto a torpor wait on a private stack word nobody wakes,
looped against a deadline in sub-hour chunks.

Found by a port, not a review: `SDL_Delay` and every frame pacer sit on
this path, and with the sentinel every delay busy-returned instantly --
burning a CPU and making timedemo timings lie.
