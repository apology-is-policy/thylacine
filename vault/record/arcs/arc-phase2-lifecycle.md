---
id: arc-phase2-lifecycle
type: arc
title: "Phase 2: the process model — Procs, Threads, and the first lifecycle"
status: active
design: ["docs/ARCHITECTURE.md"]
chunks:
  - chg-2026-05-05-p2a-process-model
  - chg-2026-05-05-p2d-rfork-exits-wait
follow-ons: []
created: 2026-08-01
---
## Goal

Establish the Plan 9 Proc/Thread pair and the minimum lifecycle over it: a
descriptor for each, a context switch, `rfork` to create, `exits` to
terminate, `wait_pid` to reap. Everything the rest of the OS schedules,
isolates, and kills is built on the shapes fixed here.

## Planned chunks

- **P2-A** — the descriptors themselves (`struct Proc`, `struct Thread`,
  `struct Context`), `cpu_switch_context` + `thread_trampoline`, the
  monotonic pid/tid spaces, and the magic-at-offset-0 double-free defence.
- **P2-Da** — `rfork(RFPROC)` + `exits` + `wait_pid`: the ALIVE→ZOMBIE→reaped
  lifecycle, the children/sibling tree, and the on_cpu spin that makes
  reaping safe.

## Close summary

This arc's record is deliberately partial: it is BACKFILLED from the
2026-08-01 sweep, which reconstructed only the two founding chunks from
`git log`. Phase 2 as a whole ran considerably wider (P2-B's EEVDF
scheduler, P2-C's SMP work-stealing, P2-E/F/G's territory / handle table /
address space), and those chunks belong to the sweeps of their own areas.
The arc stays `active` rather than `complete` because its chunk list is
incomplete, not because Phase 2 is unfinished — it landed long ago.

What Phase 2 fixed and never changed: the descriptors grow by APPENDING
with every offset asserted; `magic` sits at offset 0 so SLUB's freelist
write clobbers it; a zero-initialized descriptor is detectably invalid
(`*_STATE_INVALID == 0`); and creation consumes its identity numbers LAST,
after every fallible step, so a rollback never sparsifies the pid space.
Those four decisions are still load-bearing three phases later.
