---
id: chg-2026-10-06-untag-check
type: chg
title: "The ambush fork check accepts a fork without launchHeld"
date: 2026-10-06
arc: arc-identity-detour
commits: *(pending)*
touched:
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
Step 2 of the untag ([[chg-2026-10-06-held-untagged]]) deletes `launchHeld`
and `Launch`'s running-spawn path from the shared ambush fork, but every tree's
`ambush_fork_check` required `held_on_thylacine.go` declaring
`launchHeld = true`, so the fork change would have broken each tree's bake.
This first half moves the check ([[sub-substrate-build]]): it still refuses
`held_off_thylacine.go`, and still requires the constant where
`held_on_thylacine.go` is compiled; a fork with neither file must name
`launchHeld` in no compiled file, and its `Launch` must set `DebugHeld`, which a
fork from before the held launch does not. Main agreed on yip 0177. The fork
change itself waits for main to merge this, and main's word.
