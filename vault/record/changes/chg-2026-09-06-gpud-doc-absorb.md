---
id: chg-2026-09-06-gpud-doc-absorb
type: chg
title: "absorb docs/reference/138-gpud (retired G-1 virtio-gpu scaffold): clean redirect to sub-tapestryd + sub-substrate-gates + sub-warden"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
An already-retired historical record. gpud (usr/gpud) was absorbed by tapestryd
at G-3 and the crate + warden manifest entry were DELETED -- usr/gpud no longer
exists in the tree. Verified: the successor sub-tapestryd carries the command
machinery (gpu.rs generalized to per-surface resources), the virtio-pci:16
gather-manifest binding, AND the durable transport-pivot rationale ("the six
populated virtio-mmio slots share one page whose lifetime belongs to stratumd, so
a second persistent MMIO claimant is structurally impossible", sub-tapestryd:27-30
-- the exact G-1 measured finding). The pattern-persists gpu-gate + screendump
liveness -> sub-substrate-gates (owns tools/screendump.sh). The gather-mode bind
+ I-34 allowance narrowing -> sub-warden.

Zero-fold: every living atom already home; the G-1-specific machinery describes
deleted code and is correctly left as history. Multi-redirect stub.
