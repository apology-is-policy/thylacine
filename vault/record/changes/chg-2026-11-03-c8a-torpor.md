---
id: chg-2026-11-03-c8a-torpor
type: chg
title: "torpor keyed on the address space, a lock per bucket, oldest first, and no wake under the lock"
date: 2026-11-03
arc: arc-phase6-pouch
commits: [0e77da090]
touched:
  - sub-kernel-torpor
established: []
closed: [seam-torpor-lock-wake-spin]
opened: []
depth: skeletal
created: 2026-11-03
---
NOT LANDED. Opened ahead of the runs so the dossier pass can be carried; nothing here has been built or booted. F-9, F-10 and E-7 of the thread-model review, with `specs/torpor.tla`. Its gates, audit round and the go-build futex floor are written at the landing.
