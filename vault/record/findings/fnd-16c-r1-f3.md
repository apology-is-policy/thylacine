---
id: fnd-16c-r1-f3
type: fnd
title: "KOBJ_SRV read/write did not gate on kernel_attached"
round: adt-16c-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
hazard: haz-shared-stream-desync
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

`sys_read/write_for_proc` on a KOBJ_SRV handle whose SrvConn a kernel 9P
client owns would let userspace co-opt the kernel client's byte stream
(inject/steal frames on a load-bearing mount).

## Disposition

Fixed: both branches refuse when `srvconn_is_kernel_attached(cn)`.
