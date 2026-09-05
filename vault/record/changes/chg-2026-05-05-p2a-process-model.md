---
id: chg-2026-05-05-p2a-process-model
type: chg
title: "P2-A: the process-model bootstrap"
date: 2026-05-05
arc: arc-phase2-lifecycle
commits: ["df786930", "bbd7cbff"]
touched: [sub-kernel-proc, sub-kernel-thread]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "retro backfill -- the dossiers this chunk founded are established in this same sweep commit"
---
## What

`struct Proc`, `struct Thread`, `struct Context`; `cpu_switch_context` +
`thread_trampoline`; `proc_init`/`proc_alloc`/`proc_free`;
`thread_init`/`thread_create`/`thread_free`/`thread_switch`;
`current_thread` in `TPIDR_EL1`. Plus the R4 audit close (1 P1 + 3 P2 +
5 P3).

## Why

Everything the OS schedules, isolates or kills needs a descriptor first.
Four decisions from this chunk are still load-bearing: descriptors grow by
APPENDING with every offset `_Static_assert`ed; `magic` sits at offset 0 so
SLUB's freelist write clobbers it and a double-free is caught loudly
(R4 F42); `*_STATE_INVALID == 0` so a zero-initialized descriptor is
detectably unusable (R4 F46); and identity numbers are consumed only after
every fallible allocation step succeeds.

## Verification

Retro record, reconstructed from `git log` + the surviving code comments,
which name their own findings (R4 F42/F46, R5-F F50/F53, R5-H F79/F89).
No contemporaneous closed list was carried into the vault.
