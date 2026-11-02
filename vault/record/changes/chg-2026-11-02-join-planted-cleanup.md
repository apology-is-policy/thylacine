---
id: chg-2026-11-02-join-planted-cleanup
type: chg
title: "The F1 test's cleanup ran on an early failure, three planted points"
date: 2026-11-02
arc: arc-phase6-pouch
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-02
---
Closes what [[fnd-aux-corona-join-r2-f1]] left owed: its regression field says an early assert through the one cleanup
path had not run. Measured on 269ae1a69, one boot per plant, each probing the pts interaction pool just before and just
after the cleanup through the test's own ID-exchange hook.

| Plant | Where the early failure fired | Pool before cleanup | Pool after cleanup |
|---|---|---|---|
| P1 | hooks listed | busy | empty |
| P2 | before the conn and member link | empty | empty |
| P3 | binding live, no watch or hooks yet | busy | empty |

Each boot ended with exactly one FAIL line, under `pts.interaction_wakes_after_the_table_lock`, and no extinction but the
suite's own, so no later test inherited the fixture. All three matched the prediction written before the run. The
regreen kernel's ELF equals the base's (7ceeb5d9). The plants never reach a tree: `~/projects/aux-gate/join/plant.py`.
