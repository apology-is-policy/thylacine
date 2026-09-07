---
id: chg-2026-09-07-pouch-stratumd-boot-doc-absorb
type: chg
title: "absorb docs/reference/86-pouch-stratumd-boot (the Phase 5/6 boot path): clean multi-redirect, all surfaces covered"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
pouch-stratumd-boot (the Phase 5/6 sub-chunk 16a/16b/16c boot path: joey spawns
stratumd -> pool mount over in-process virtio-blk -> kernel 9P client attaches to
/srv -> /sysroot mount -> ramfs pivot -> stub retire). A 2189-line chunk narrative
spanning many surfaces. Verified atom-by-atom against the vault.

ALL LOAD-BEARING ATOMS VERIFIED COVERED (a clean multi-redirect, zero fold):
- The boot sequence spine -> sub-stratum-boot (audit:hard, fresh 2026-09-06;
  title "Bringup -- spawn, wait for an event, attach, pivot"; 52 stratumd/joey/spawn
  + 19 9P/srv/socket/attach + 15 pivot/sysroot/ramfs/stub hits) -- the PRIMARY home,
  incl. the stratumd-as-driver architecture + the /srv-socket-is-readiness signal.
- The in-process block driver (bdev_thylacine) -> sub-stratum-bdev (audit:hard).
- joey's spawn orchestration -> sub-kernel-joey (audit:hard).
- The spawn ABI: SYS_SPAWN_FULL_ARGV argv pass-through -> sub-kernel-syscall-dispatch
  + sub-kernel-exec + sub-pouch-process; CAP_HW_CREATE grant -> sub-kernel-caps
  (+ sub-stratum-boot + sub-warden).
- The SrvConn + kernel_attached gate + SYS_ATTACH_9P_SRV (16c) -> sub-kernel-srvconn
  (audit:hard, 70 kernel_attached/SrvConn hits).
- The 9P srvconn transport + client + dev9p -> sub-kernel-ninep-client/-transport/
  -dev9p (all audit:hard).
- SYS_PIVOT_ROOT + /sysroot + I-1 -> sub-kernel-territory (audit:hard, 19 pivot hits).
- The ramfs stub -> sub-kernel-content (devramfs).

The Stratum src/ code the doc cites is carried by the vault's own STRATUM area
(sub-stratum-boot/sub-stratum-bdev), not out of scope. Big multi-redirect stub
organized around the boot stages. Zero code change.
