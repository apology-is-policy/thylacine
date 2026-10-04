---
id: chg-2026-10-04-hidden-storage-qualification
type: chg
title: "Verify all fifty hidden-storage SMP and UBSan boots"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-tapestryd, sub-libtapestry, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Implementation8b2212c0e completed default SMP1/4/8 and UBSan SMP4/8,
ten boots per configuration. Every individual result and all five summaries
were verified:50PASS, zero corruption/external-kill/inject-miss/timing/other.
Runner exit0, source/index and original protected draft hashes were checked;
Mac was released. Evidence: work/oct4-hidden-storage/matrix-verified.json.

Blind to graphical workloads, Pi/backend fence correctness, minimum displays
and the real Halcyon failed-reveal notice. No ASan or Main landing claim;
clipboard/application activation remains open. Passing raw per-boot logs are
overwritten by the standard harness; outcomes, timings and final log survive.
Single-agent verification, not an independent review.
