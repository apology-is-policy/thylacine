---
id: chg-2026-11-04-dormant-answers-estale
type: chg
title: "A dormant present and HIN1's BadHandle answer ESTALE"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-tapestryd, sub-libhalcyon, sub-libthyla-rs, abi-errno]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/err.rs"
  - "usr/lib/pouch/patches/0001-pouch-syscall-seam.patch"
depth: skeletal
created: 2026-11-04
---
Vote 66, [[dec-2026-11-04-dormant-answers-estale]], closing merge audit r3's F1. `tapestryd`'s present on a dormant pixel generation and HIN1's `BadHandle` answer `ESTALE`, because the kernel reads a server's `EBADF` as `EIO`. `abi-errno` gains no value: 116 is already in the registry and in `err.rs`, and patch 0001's range contract already covers it.
