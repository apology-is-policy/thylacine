---
id: chg-2026-09-25-pty-interaction-qualification
type: chg
title: "Qualify terminal ownership fronts and concurrent retirement"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-pts, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Extends the kernel ownership regressions with real service-transport identity,
native marshalling faults, bounded WATCH allocation rollback, counter exhaustion
and concurrent retirement/last-watcher-close/rebind. One fixture initially omitted
the poster identity and was correctly refused; corrected full boot passes 1698/1698.
Test-only seams do not extend the syscall ABI. The status and kernel review retain
exact evidence and the outstanding repeated SMP, production and live-client work.
