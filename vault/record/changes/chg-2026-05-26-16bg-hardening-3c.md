---
id: chg-2026-05-26-16bg-hardening-3c
type: chg
title: "P6 hardening #3c: the gated mallocng corruption dump"
date: 2026-05-26
arc: arc-pouch-boot
commits: ["a120bf92"]
touched:
  - sub-pouch-process
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
A `POUCH_MALLOCNG_DIAG`-gated dump inside `enframe`: sizeclass, slot
index, maplen, stride, and the 16 bytes around `p` -- emitted BEFORE the
assertion fires, through a direct `SYS_write` so it survives a corrupted
heap and involves no allocation. Off by default; on the happy path the
branch short-circuits and produces nothing.
