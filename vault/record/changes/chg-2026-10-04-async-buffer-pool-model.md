---
id: chg-2026-10-04-async-buffer-pool-model
type: chg
title: "Model payload ownership separately from completion consumption"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom]
established: [spec-loom-service-buffers]
closed: []
opened: []
depth: skeletal
---
Two clean finite configurations and eleven named counterexamples establish the
bounded design gate before runtime consumers. Retirement has no consumer or
peer fairness premise. Legacy Loom model fingerprints stay unchanged. This is
single-agent self-review and abstract evidence, not enabled kernel support.
