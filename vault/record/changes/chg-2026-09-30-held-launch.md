---
id: chg-2026-09-30-held-launch
type: chg
title: "The held launch: ambush spawns held where the kernel has the hold"
date: 2026-09-30
arc: arc-go-ide
commits: ["f4924564"]
touched:
  - sub-substrate-build
  - sub-kernel-birth-hold
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-30
---
The userspace half of the birth hold ([[chg-2026-09-29-birth-hold]]). The Go
fork's `SysProcAttr.DebugHeld` sets `SPAWN_DEBUG_HELD`, and ambush's `Launch`
spawns held when built with `-tags thylacine_held`. `tools/build.sh` passes the
tag to both ambush binaries, the ramfs copy for `/ambush-probe` and the
`/goroot/bin` copy nora's `:debug` runs, because this tree's kernel carries the
hold ([[sub-substrate-build]], [[sub-kernel-birth-hold]]). The tag exists
because both forks are shared with trees whose kernels refuse the flag; it goes
when every tree carries the hold. The build refuses a fork without the held
launch and reads the tags back from each binary. `Launch` writes `exitkill`
before `stop`, because the stop's conversion ends the orphan rule's cover, and
`/ambush-probe` stage C now requires the launch stop's PC to be the program's
ELF entry, which tells a held launch from a raced one on every boot, and stage D
requires a launch that fails on a program dying as it loads to reap that child,
which a held spawn hands back already dead, and the debugger's kill of a target
already killed from outside to succeed and reap it. This closes the Delve launch
race behind stage C's three boot-fatal sightings. The audit also found the Go
fork's spawn record holding stack addresses as integers across calls that can
move the stack; those fields are pointers now, and the spawn-args mirror check
requires it, with every 8-byte kernel field classified as an address or not. The
Go fork's `Dir` handling still moves the whole parent; its fix is an ABI change,
queued for a vote. Verified on the landed tree, with the forks at go-thylacine
4aba404 and ambush ce9154d: `tools/test.sh` 1782/1782, `/ambush-probe` stages A
to D (stage D `reaped=1 killed=1`), `dap-nora`, `nora-demo`, and
`tools/ci-smp-gate.sh` 5 rows x 10/10 with no corruption.
