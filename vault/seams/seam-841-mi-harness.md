---
id: seam-841-mi-harness
type: seam
title: "The deterministic multi-in-flight cross-Proc-death client harness"
status: open
surface: [sub-kernel-ninep-client]
opened-by: chg-2026-06-03-841-pipeline
tracker: "standing (carried across #841/#845/#349/#375/#52-#53/Loom/8c-3/#90)"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

A deterministic two-thread-same-client / loopback-fake-server harness
driving ≥3 concurrent in-flight ops with scripted death, election races, and
back-pressure. It owns, accumulated across the lineage: the pre-fix failures
of the #841 F1/F6/F7 class; the live `client_run` DIED → Tflush → survivor
Rflush drain; the live park→retry→complete loop of `client_send_flow`
(including the park-arm spill drive); the clunk_async never-sent arm and the
DIED-path rollback; a live reader-death mid-frame with a survivor taking the
role; a debugged reader-role holder at stop-time with a concurrent
survivor's op completing.

## What closes it

An mq/loopback-based harness on `9p_transport_mq.c` with scripted
multi-thread drivers (originally planned to land with the A-5b multi-user
workload). Each covered case should fail deterministically on its pre-fix
code — the durable-regression bar.

## Risk while open

The SMP-window interleavings of the shared client are validated by
prosecution rounds + reasoning + [[gate-smp]] sampling, not deterministic
regression — a regression in these paths would surface as rare
gate-corruption rather than a named test failure.
