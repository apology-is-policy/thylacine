---
id: chg-2026-05-05-p2d-rfork-exits-wait
type: chg
title: "P2-Da: rfork(RFPROC) + exits + wait_pid — the first lifecycle"
date: 2026-05-05
arc: arc-phase2-lifecycle
commits: ["4465408c", "9b8761f9"]
touched: [sub-kernel-proc, sub-kernel-death]
established: []
closed: []
opened: [seam-rfork-flags-unimplemented]
mirrors-checked: []
depth: skeletal
no-dossier-change: "retro backfill -- the dossiers this chunk founded are established in this same sweep commit"
---
## What

The multi-process lifecycle: `rfork` creating a Proc with one initial
Thread, `exits` transitioning it to ZOMBIE, `wait_pid` reaping a zombie
child. The parent/children/sibling tree. Plus the follow-up that made the
reap safe: `wait_pid` spins on each Thread's `on_cpu` before `thread_free`.

## Why

Plan 9's `rfork` unifies fork and thread-create behind one flag word; only
`RFPROC` was implemented, with every other flag extincting rather than
silently ignoring — a choice that still holds and is recorded as
[[seam-rfork-flags-unimplemented]].

The `on_cpu` spin is the first appearance of the property that dominates
this area: a Thread's run STATE does not tell you whether a CPU is still
executing on its stack. Reaping without the spin frees a descriptor a peer
is mid-`cpu_switch_context` into. The same insight later had to be applied
inside `thread_free` itself (#788).

## Verification

Retro record from `git log`. The contemporaneous audit rounds are not
carried into the vault; the code comments retain their finding ids.
