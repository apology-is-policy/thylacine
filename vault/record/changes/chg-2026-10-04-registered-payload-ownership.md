---
id: chg-2026-10-04-registered-payload-ownership
type: chg
title: "Make raw registration unsafe and bound client payload borrows"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-libthyla-rs, sub-libtapestry, sub-mechanism-drivers, sub-net-clients, sub-netperf, sub-kernel-territory, abi-loom-ring]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/loom.rs: shared ABI layouts unchanged; unsafe is a Rust API qualifier"
  - "kernel/include/thylacine/loom.h: no structure, flag or syscall changes"
depth: skeletal
---
AS-R8 raw boundary and existing callers now respect asynchronous range ownership:
completion identity/phase precedes Tapestry borrows, and unresolved Weft waits
refuse slice access. Actual module compile-fail/runtime tests,27 Tapestry tests,
six source mutants, native1830/1830 and graphical media/F10 SAK pass. Captures are
1280x800; no minimum-display/Pi/new clipboard claim. Owned safe pool clients and
broad qualification remain owed. Single-agent self-review, protected drafts kept.
