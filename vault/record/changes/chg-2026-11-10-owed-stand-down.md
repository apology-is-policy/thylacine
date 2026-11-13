---
id: chg-2026-11-10-owed-stand-down
type: chg
title: "The owed-free pass stands down for one window only after a stall its own repeat met; any other stall leaves it and its barrier up, so it gets the first turn after every other revival"
date: 2026-11-10
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
created: 2026-11-10
---
Supersedes chg-2026-11-10-owed-quiet. Vote 81 audit round 9 (Opus 5.5 on Opus 5.5) on the quiet gate: 0 P0, 1 P1, 0 P2, 3 P3, no soundness defect. F1 (P1): the gate stood the owed-free pass and its barrier down after every stall, for `OWED_QUIET_MS` past its revival, with no bound. One slow synchronous command per revival kept the pass from ever seeing a quiet engine, and the lowered barrier released the owner's parked chains at each revival, which made the next command slow. Every owed free stayed unpaid and the barrier stayed lifted for as long as such a workload ran, and the owner's own rendering is such a workload: round 7's F1 harm (uncharged pins, retiring ledger entries) returned. The pass now stands down only after a stall one of its own commands met (`Gpu::owed_stood_down`, `gpu_api::owed_stands_down`), until another stall begins or the engine has run `OWED_QUIET_MS` since that stall revived. Any other stall leaves the pass and the barrier up: fenced submissions are refused while the engine is stalled, so at the revival the held chains stay parked and the pass sends first, into a queue the stall emptied. The pass gets the first turn after every revival except those that follow its own stall, and each of those goes to the requests and the held chains (round 8's F4 and its self-found twin stay closed). F2 (P3, also self-found): BUSY's construction compared stalls only from the destroy, so a revival just before it could send the held chain to the device and read a correct build as M19; stalls are now counted from the attempt's creates, the defer is armed `OWED_QUIET_MS` and a margin after them, and a stall while the free is due makes the attempt unconstructed. F3 (P3): the uncharge-honesty claim (tapestryd's vindication comment, sub-tapestryd, warp-prove) and three GPU-DESIGN and sub-lictor passages on when a repeat runs now name the stand-down; `Gpu::owed_ids` is renamed `barrier_ids`, since it answers empty while frees are owed. F4 (P3): the gate's wiring had no runtime witness. The new test lever `Request::TestOwedStall` (opcode 73; tapestryd's bo ctl verb `stall-repay`, the caller's own BO, four shots a boot, each gone in 2 s) rides one stall shot on the pass's next repeat of that BO's free. The readback leg's new YIELD arm uses it and passes only if the free is paid no sooner than a window after the repeat's stall. FREE now requires its repeat within 300 ms, since a stall that met the client's free leaves the pass up. Pi mutants: M22 (the pass never stands down), YIELD=FAIL; M23 (the stand-down ends only at another stall), YIELD=FAIL; M24 (every stall stands the pass down, the round-9 gate), FREE=FAIL. Separately, a reading of a suspected unchecked HOST3D ring free (OPEN-BUGS 2026-11-10T05:01Z) found no guest-safety defect, and GPU-DESIGN 4.5.15 now says why a ring's free waits on client mappings rather than chains.
