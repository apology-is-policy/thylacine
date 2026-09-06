---
id: chg-2026-09-06-devdev-absorb
type: chg
title: "close the entry cluster: author sub-kernel-joey over the kernel/joey.c orphan (2nd orphan resolved) + fold the /dev/pts graft into sub-stratum-boot + stub 109-devdev (multi-redirect devdev/cons/joey/stratum-boot/territory) -- 65 absorbed / 92 live, BOTH entry orphans now resolved"
date: 2026-09-06
arc: arc-vault
commits: ["77f766f3"]
touched: [sub-stratum-boot]
established: [sub-kernel-joey]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The last piece of the entry/trivial-devices cluster. Three deliverables.

**Authored sub-kernel-joey** (parent moc-kernel-boot, audit:hard, code:
kernel/joey.c + joey.h, guarded-by [inv-i2, inv-i27]) over the kernel/joey.c
orphan -- the 2nd and last entry-cluster orphan, from a full read. It is the
kernel-to-userspace handoff: joey_run builds the boot namespace via
joey_mount_static_dev (the /srv idiom generalized -- /srv/proc/ctl/dev/hw/hw-pci/
env, inherited by descendants via territory_clone, dropped by the pivot), loads
/joey from the initrd as a #85 exec-window transient (freed on both exec arms),
and rforks it with CAP_ALL as the I-2 capability-delegate root; joey_thunk stamps
the trust roots (console-attached + owner [I-27 anchor], may-post-service [A-5b],
init [orphan adopter], name) in the child's own context before exec; the wait is
BY PID (#94, the orphan-adoption race would reap the wrong Proc + mis-diagnose a
failed boot). audit:hard for the I-27 console anchor + the I-2 delegate root.

**Folded the /dev/pts graft into sub-stratum-boot.** The Explore found its mount
seq documents the seven carried pre-pivot handles but not the /dev/pts graft --
a separate post-swap mount (the ptyfs tree does not exist until joey spawns
/sbin/ptyfs; then a fresh open-is-connect of /srv/ptyfs is MREPL-mounted over the
devdev pts stub, no mkdir, boot-fatal on failure).

**Stubbed 109-devdev** (multi-redirect: sub-kernel-devdev for the front door +
I-27 gate + the revoke-asymmetry [folded prior]; sub-kernel-cons for the shared
console API; sub-kernel-joey for the kernel boot mount; sub-stratum-boot for the
post-pivot re-graft + /dev/pts; sub-kernel-territory + seam-80 for the
mount-table sizing). "What it got wrong": the PGRP_MAX_MOUNTS 8->12/#80 figures
are stale (now 32), and its "devrandom no longer reachable by any path" is
imprecise (still registered + boot-seeds; only its read path is superseded).

BOTH entry-cluster orphans (uart.c @418035f0, joey.c here) now resolved. No code
touched; no audit owed. sub-stratum-boot already at 2026-09-06. view-absorption:
64 -> 65 absorbed, 92 live.
