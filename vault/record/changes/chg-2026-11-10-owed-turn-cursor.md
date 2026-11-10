---
id: chg-2026-11-10-owed-turn-cursor
type: chg
title: "A parked context cut short in the owed-free pass's turn starts the next turn, and a turn looks only at frees that hold a chain back"
date: 2026-11-10
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-10
---
Supersedes chg-2026-11-10-owed-turn-budget. Vote 81 audit round 13 (Opus 5.5 on Opus 5.5) on the turn's own looks: 0 P0, 0 P1, 0 P2, 2 P3, no soundness defect. F1: a turn's sixteen looks are shared by every context it visits in a pass, and running out inside a context moved the cursor past it even when the contexts before it had spent most of the looks, so a context cut short went last in the next pass too and was cut short again, for as long as an earlier context's frees waited on its chains on the device; the docs said a context waits once per context with a parked chain. Now a context cut short sets the cursor to the context before it, so the next turn starts at it with every look; one that had every look still moves the cursor past it. F2: the cursor was documented as the context last served; it is the context the next turn starts after. Self-found in the round: a turn counted every free a parked stream's context holds, including one a poisoned chain reaches, which holds no chain back (`OwedReach::of` drops it) and stays unclear for as long as the poison; now a turn looks only at frees in the barrier (`OwedReach::bars`). The auditor showed the case cannot arise today, since poisoning is keyed by the client's context and releases every parked chain of it, and tapestryd links a client's resources to no other client's fenced context; the round-12 record's case of frees refused by the poison of another of the client's contexts is that one. The filter keeps the turn's bound from resting on either premise.
