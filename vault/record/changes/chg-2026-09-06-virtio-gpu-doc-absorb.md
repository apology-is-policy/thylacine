---
id: chg-2026-09-06-virtio-gpu-doc-absorb
type: chg
title: "absorb docs/reference/43-virtio-gpu (P4-L probe driver): zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["86e027c3"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/43-virtio-gpu.md -> ABSORBED

Absorbed the 325-line virtio-gpu reference doc into a redirect stub. The P4-L
/virtio-gpu probe driver (the composed MMIO/DMA/IRQ substrate, DeviceID 16, the
config-space, the 2D scanout six-OK contract) -> sub-virtio-probes (owns
usr/virtio-gpu/src/main.rs); the production GPU compositor that superseded it ->
sub-tapestryd. Zero fold.

101 -> 102 absorbed of 157. lint 0-fail.
