---
id: chg-2026-11-10-owed-turn-budget
type: chg
title: "A parked context's turn in the owed-free pass has its own looks, the rotation its own, and a turn out of looks moves past its context"
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
Supersedes chg-2026-11-10-owed-barrier-holders. Vote 81 audit round 12 (Opus 5.5 on Opus 5.5) on the holder release and the parked contexts' turn: 0 P0, 0 P1, 0 P2, 1 P3, convergent with a self-found item, disposed as P2. `gpu_api::OwedPass` let a turn spend the pass's whole examined share: a context with a parked chain whose first sixteen waited frees were all held (by its own chain still on the device, or refused because a poisoned chain of another of its client's contexts reaches them) ended every pass with no free tried, the rotation never ran, and since the cursor moved only on a pick no other context ever had its turn; with the poison, the context could stay parked behind a free it held further back for as long as the poison lasted. Now a turn looks at `OWED_FREES_EXAMINED` frees at most, the rotation has its own share, set once the turn's pick is out so the pick (put back behind the rest if kept) is not looked at again in that pass, and a turn out of looks inside a context moves the cursor past it.
