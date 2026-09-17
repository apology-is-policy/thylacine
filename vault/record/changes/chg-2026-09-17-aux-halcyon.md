---
id: chg-2026-09-17-aux-halcyon
type: chg
title: "Aux integration: ordered media, native panes, manual, and bounded interrupt/transport ownership"
date: 2026-09-17
arc: arc-tapestry
commits: ["f82c598d"]
touched: [sub-halcyond, sub-view, sub-gallery, sub-manual, sub-sdl-port, sub-kernel-pci-irq, sub-netd-server, sub-netperf, sub-dosbox, sub-nocturne-tools, sub-kernel-exception, sub-kernel-boot-sequence, sub-kernel-devdev, sub-netdev]
established: []
closed: []
opened: []
mirrors-checked: [abi-boot-banner]
depth: rich
created: 2026-09-17
---

The operator authorized integrating committed aux work into main, retaining
single-agent review, and separately approved the PCI interrupt and TCP close
contracts. The detailed review is `docs/AUX-HALCYON-INTEGRATION.md`; the run
narrative is `docs/JOURNAL.md` (2026-09-17). The full SMP matrix and the additional eight-CPU ITS check pass.

## Contract changes

[[sub-view]] and [[sub-halcyond]] correlate out-of-band raster placement with
an inline Beacon object, preserving command/transcript order within bounded
per-tile ownership. [[sub-gallery]] composites alpha over black and titles its
native pane. [[sub-manual]] checks and renders six installed sections through
Beacon or plain text. [[sub-sdl-port]] forwards SDL titles and restores title
and dynamic-frame intent when video-mode changes recreate a surface.

[[sub-kernel-pci-irq]] replaces a device-order assumption with function-owned
shared INTx endpoints and protected MSI-X routing. GICv2m and ITS/LPI are both
implemented; uncertain hardware retirement quarantines routing/storage rather
than returning it to an allocator. [[dec-2026-09-17-pci-interrupt-domains]]
records the operator's design decision.

[[sub-netd-server]] gives closing TCP transports a private bounded owner after
public slot release. The last clunk immediately detaches Weft and permits slot
reuse. Admission and expiry are diagnosed; [[sub-netperf]] includes admission
waiting in its latency. [[dec-2026-09-17-tcp-transport-retirement]] records the
measured byte loss and approved repair.

## Evidence and limits

Three controller configurations pass 1,570 kernel tests and byte-exact receipt
of 8 MiB after immediate close. Twelve audio cases pass across those backends.
The post-repair manual/Haul/DOSBox batch passes all eight scenarios, with Haul
using a real Pi npxf fixture. Halcyon's media gate passes after the repair;
the corrected DOSBox session gate passes in 71 seconds with reviewed pane,
zoom and shell-return captures. The full default/UBSan by SMP4/SMP8 matrix passes 40/40 with no failure
classifications. The additional eight-CPU ITS/TCG UBSan boot passes in 121
seconds with all 1,570 kernel tests.

This is a self-review record, not an independent audit. The approved Lex
curiata visual specification does not imply an implemented trusted GPU sink.
The pointer-only driver open interface remains documented architectural debt,
although the per-operation repair now preserves bounded 9P open errno.
