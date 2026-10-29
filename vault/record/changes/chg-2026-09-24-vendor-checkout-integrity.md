---
id: chg-2026-09-24-vendor-checkout-integrity
type: chg
title: "Preserve vendored source files across fresh checkouts"
date: 2026-09-24
arc: arc-astra-halcyon-followup
commits: ["6e1ba9a6"]
touched: [sub-substrate-build]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
A fresh worktree could not build because generic ignore rules excluded two
Cargo-checksummed source files. Restore them, plus the two similarly ignored
libsodium MSVC build scripts identified by Aux, and exempt the exact vendored
paths. Both Rust files match their upstream Cargo SHA-256 manifests. The
fresh Halcyon image builds; no ignored files remain under `third_party/` in
the repaired checkout. The automatic vendor-ignore guard is owned by Aux.
