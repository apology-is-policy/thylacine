---
id: chg-2026-11-09-sak-renew
type: chg
title: "A takeover behind a GPU backlog renews the kernel's quiesce deadline (vote 71)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-proc
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked:
  - kernel/include/thylacine/seat.h
  - kernel/include/thylacine/syscall.h
  - kernel/include/thylacine/proc.h
  - kernel/syscall.c
  - usr/lictor/src/endpoint.rs
  - usr/lib/libthyla-rs/src/lib.rs
  - usr/lib/libt/include/thyla/syscall.h
  - kernel/include/thylacine/vivarium.h
depth: skeletal
created: 2026-11-09
---
Operator vote 71 ([[dec-2026-11-05-sak-waits-on-backlog]]). seat.h adds `SEAT_RENEW` (13) and the two bounds, `SEAT_QUIESCE_NS` (5 s) and `SEAT_QUIESCE_LIMIT_NS` (35 s: the backend's 30-s abandonment plus one window). The chord sets the deadline and a limit; RENEW, from the service, for its generation, while QUIESCING, moves the deadline to now + 5 s capped at the limit, and refuses once the limit is spent; an expired quiesce has already failed at the op's preamble, so a renewal never revives one. `sys_trusted_seat_handler` admits op 13 (its bound was `op > SEAT_CLIENT`). Lictor's takeover renews at most once a second while `Gpu::retired_mark` (the controlq's used index) moves and the engine is alive, and says "takeover renewed behind a draining backlog". Kernel test `cons.graphical_seat_renew`: the service renews inside the limit, Corvus and another generation cannot, the cap holds, a spent limit refuses, an expired quiesce fails, an exclusive seat refuses. Scenario `ls-graphical-sak-backlog`: three chords behind `warp-prove chain-hold 240 deep` must each reach the trusted scanout after at least one renewal (INCONCLUSIVE with none); its fake-guest rows read 9 of 9 as designed. The mirrors: libthyla-rs, libt, vivarium.h and syscall.h carry the syscall number and spawn permissions only, not the op codes, so they are unchanged.
