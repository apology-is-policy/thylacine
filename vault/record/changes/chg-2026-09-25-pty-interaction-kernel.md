---
id: chg-2026-09-25-pty-interaction-kernel
type: chg
title: "Implement bounded terminal ownership observations and lifecycle revocation"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-pts, sub-kernel-proc, sub-kernel-death, sub-kernel-jobctl, sub-kernel-caps, sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-libthyla-rs, lock-pts, lock-proc-table, abi-errno, abi-pty-interaction]
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/syscall.h, usr/lib/libt/include/thyla/syscall.h, usr/lib/libthyla-rs/src/pty_interaction.rs, usr/lib/libthyla-rs/src/err.rs, usr/lib/pouch/patches/0001-pouch-syscall-seam.patch]
depth: skeletal
created: 2026-09-25
---
Implements SYS_PTY_REGISTER ownership suboperations, bounded anonymous watch
Spoors, fresh combined membership/epoch admission and lifecycle revocation on
Aux's cleared `0cb5b244` base. No allocation or uaccess runs under lifecycle/pts;
post-unlock wake pins prevent retired pool reuse. ENOSPC=28 gains its Rust mirror.
QEMU kernel regressions cover role/seal checks, direct foreground changes,
watcher capacity and actual exec/death retirement. Review is single-agent under
the operator's direction. The interaction status records the exact evidence and
remaining SMP, syscall-front and live-client obligations; this is no live
clipboard or Main landing.
