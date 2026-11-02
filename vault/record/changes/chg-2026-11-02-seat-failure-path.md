---
id: chg-2026-11-02-seat-failure-path
type: chg
title: "A seat that fails says so: votes 36 and 37, the generation retire, and the two defects behind one lost boot"
date: 2026-11-02
arc: arc-halcyon-interaction
commits: ["ff4a7458e", "ee99f3c70", "9cc84c31e", "310355dc6", "e4c2dc5bc", "99e70535f", "5c325e53a", "adf79f457", "65a430ec8", "c3f024eaa", "5a188d90d", "dba45c3c4", "0d2ba9352"]
touched:
  - sub-corvus
  - sub-halcyond
  - sub-kernel-caps
  - sub-kernel-death
  - sub-kernel-mm-phys
  - sub-kernel-proc
  - sub-kernel-syscall-dispatch
  - sub-libhalcyon
  - sub-lictor
  - sub-substrate-interactive
  - sub-substrate-kernel-suite
  - abi-trusted-seat
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/seat.h, kernel/include/thylacine/syscall.h, kernel/include/thylacine/proc.h, kernel/syscall.c, usr/lictor/src/endpoint.rs, usr/lib/libthyla-rs/src/lib.rs, usr/lib/libt/include/thyla/syscall.h, kernel/include/thylacine/vivarium.h]
depth: skeletal
created: 2026-11-02
---
A dead seat service now fails the seat and returns it to normal
([[dec-2026-10-29-seat-service-death]], vote 36), and `SEAT_QUERY` states the
episode's medium ([[dec-2026-11-02-seat-episode-medium]], vote 37). The audit
round on those found that vote 36 made a false success deterministic:
`EpisodeIo::finish` read normal at its generation as a commit, and a service
death in restoring cancels the grant and sets normal in one lock hold. So a
return to normal through a failure now retires its generation and a commit
keeps it ([[dec-2026-11-02-seat-failure-retires-generation]]).

That fix killed the session, about half of recoveries, because the proposal had
not checked the consumers above Lictor. Lictor's failed arm stored the phase and
not the generation, so the state page served the old generation at normal for
one pass, Halcyond's link joined at it, the table refused, and a refused join
was fatal. A recovery now reports phase and generation together, and a joined
participant re-samples a refused join once (`Refusals`, `Seat::adopt`).

Hunting one lost boot found two more defects, both of which the gate wrapper's
retry would have hidden. caps-probe wrote its seat witness in three console
writes, so another process's line could land inside it and a scenario needle
missed: the probe is now one write. An exact free-page round trip in the kernel
tests read `phys_free_pages`, which excludes the per-CPU magazines, so a refill
inside the window read as 8 pages leaked and extinguished the boot on a green
kernel: those tests now settle the magazines first (15 exact round trips plus
one bound in test_capacity; 65a430ec8's message miscounts them as 16). The
wrapper's last lines now name a scenario that passed only on a retry, and
`ls-graphical-sak-soak` cycles the seat's return with no lever.

Open: the four P3s of the third audit round and the one-write batch
(corona/seat-p3), F4's Corvus half (no guest drives `EpisodeIo` through a seat
failing right after ACK), and a chord within Corvus's 10 ms poll after a commit,
which answers INTERNAL (fail closed).
