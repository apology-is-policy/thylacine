---
id: chg-2026-10-06-held-untagged
type: chg
title: "The held launch drops its build tag: every ambush build spawns held"
date: 2026-10-06
arc: arc-go-ide
commits: ["16348c645"]
touched:
  - sub-substrate-build
  - sub-kernel-birth-hold
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
The held launch ([[chg-2026-09-30-held-launch]]) rode the `thylacine_held`
build tag while some trees' kernels lacked the birth hold and refused the flag.
Main carries the hold since its aux-3 merge, so ambush 073faaa compiles the held
launch untagged, and `tools/build.sh` stops passing the tag: `AMBUSH_TAGS` and
`ambush_artifact_check` go, and `ambush_fork_check` asks the untagged build's
file selection, refusing an older fork, which would compile the running spawn
([[sub-substrate-build]], [[sub-kernel-birth-hold]]). Step 1 of two, compatible
with a tree that still passes the tag; step 2 deletes `launchHeld` and
`Launch`'s running-spawn path once no tree's build script checks for the
constant. Main cleared it on yip 0172.
