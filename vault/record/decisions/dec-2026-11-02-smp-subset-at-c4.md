---
id: dec-2026-11-02-smp-subset-at-c4
type: dec
title: "C4 lands on an SMP amplifier subset under the operator's CI hold"
date: 2026-11-02
status: standing
decided-by: user-vote
affects: [sub-kernel-notes]
created: 2026-11-02
---
## Fork

The operator's hold of 2026-11-02 forbids the full CI gate until told otherwise, including `tools/ci-smp-gate.sh`'s full matrix, and C4 changes a wake path.

## Decision

Vote 17 of 2026-11-02: `SMP_GATE_CONFIGS="default-smp4 ubsan-smp4"`, 20 boots, under a normal Mac lease when C4 lands. The landing record says the full matrix did not run.
