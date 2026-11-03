---
id: chg-2026-11-03-seat-session-start
type: chg
title: "A chord at session start is a wait: the seat's P3s, a member that may owe, and a needle checker"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["fc61093a2", "659023862", "c263b9080", "46d65fb35", "70e76dff2", "4227bfde1"]
touched:
  - sub-halcyond
  - sub-libhalcyon
  - sub-libtapestry
  - sub-libthyla-rs
  - sub-lictor
  - sub-tapestryd
  - sub-substrate-machine
  - sub-substrate-interactive
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-03
---
A seat away from normal at session start is now a wait and not a failure. A
refused Reserve retries on the same fid, a first Join a chord refuses samples
through `Coordinator::sample` and joins after the episode, and bring-up, setup and
an unreported flight stop counting while the seat is parked. That stops at
`PARKED_CEILING_MS` (120 s), since the kernel gives FAILED no deadline and a held
key keeps it there, so a session that cannot start still reaches login's fallback.

A member that may owe a cancellation dies on any failure, and it may owe from the
send of a Join, because the table serves one before its reply can be read. Only a
refused first Join clears that. Reading the rule as "the Link has read the reply"
was this batch's own regression, found by the audit round and by the self-audit
before the first compile.

A refused chord clears its batch and pushes a reset. `seat_control::Participant`
and `Interaction::age` put these decisions under host tests.
`tools/check-console-lines.py`, run by `build.sh`, fails a scenario needle that
reads across two console writes, and the lines it found are one write now.
`test_free_pages_settled` reads until two reads agree, and the exact round trips
in five more test files use it. [[abi-boot-banner]] gains three scenarios as
consumers, and no banner or extinction text changes.

Open: `reserve`'s parked write and `Client::call` to Lictor have no bound; `age`'s
guest wiring has no witness; 69 lines of all-variable text write their newline
separately.
