---
id: chg-2026-09-29-birth-hold
type: chg
title: "The birth hold: a spawned child parked before its first instruction"
date: 2026-09-29
arc: arc-go-ide
commits: ["a9596fb5"]
touched:
  - sub-kernel-birth-hold
  - sub-kernel-exception
  - sub-kernel-devproc
  - sub-kernel-death
  - sub-kernel-jobctl
  - sub-kernel-proc
  - sub-kernel-exec
  - sub-kernel-caps
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-substrate-build
  - sub-libthyla-rs
  - sub-pouch-process
  - sub-viv
  - spec-debug-stop
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
Delve's launch raced its child: the probe's program reached its loop before
the debugger's attach and stop landed, so an entry breakpoint never fired
(DELVE-PORT-DESIGN 8c-4, closure (b), "it bit"). The operator voted the shape
on 2026-09-29: a spawn flag, and a held child whose spawner dies before taking
it over is killed. `SPAWN_DEBUG_HELD`, in the spawn record's last reserved
slot, returns the pid only once the child has loaded and parked in front of
its first instruction; the owner's `stop` converts the hold into a debug stop,
`start` and an explicit `detach` release it, and the orphan rule kills a hold
nobody took over ([[sub-kernel-birth-hold]]). The child enters EL0 through a
new routine that builds its first frame and runs the ordinary return over it
([[sub-kernel-exception]]).

The spec found a real window on its first held run: the EXITKILL release
terminates and only then clears the stop, so a parked thread could read the
cleared flag and eret into its own death. Both parks now re-check death after
their wake condition ([[spec-debug-stop]]). Audit round 1 (Fable 5.1) found one
P1: the first draft answered an interrupt latched at the birth park by re-running
the checkpoint, which spun with interrupts masked once note delivery declined a
debugger-written stack pointer. The park now ends the child itself, and the
model keeps both wrong answers as buggy configurations. Round 2 (Fable 5.1)
found nothing above P3: its four sharpened the claims (a held child's thread
masks nothing, so the park's latch exit always applies), pinned the exit
message in the tests, and closed a gap in the mirror check. A build step now checks
every copy of the spawn record against the kernel's layout
([[sub-kernel-syscall-abi]], [[sub-substrate-build]]). The Go fork's
`SysProcAttr` and ambush's `Launch` wait on the operator, so Delve's launch
still races until they land.
