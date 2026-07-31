---
id: fnd-stalk3b-r1-f1
type: fnd
title: "The kernel_attached I/O guard did not follow the conn endpoint from KObj_Srv to KOBJ_SPOOR"
round: adt-stalk3b-r1
severity: P2
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
hazard: haz-shared-stream-desync
fixed-by: chg-2026-06-03-stalk3b-open-connect
regression: devsrv.kernel_attached_io_refused
created: 2026-07-31
---
## Prosecution

The 16c "no direct I/O on a kernel-attached conn" guard lived on the
`sys_read/write_for_proc` KOBJ_SRV arms — but C1 retargeted the
byte-conn endpoint to a KOBJ_SPOOR CSRVCLIENT Spoor, so `t_read`/
`t_write` now route through `devsrv_read`/`devsrv_write`'s CSRVCLIENT
branches, which had NO guard. After `SYS_ATTACH_9P_SRV` wraps the conn,
a Proc still holding the conn-Spoor handle that reads it drains
Rread/Rwalk bytes meant for the kernel 9P client; a write interleaves
out-of-band bytes into the request stream — 9P wire corruption on a
load-bearing mount. Latent-only because the endpoint is non-dup-able and
transferless (blast radius = the attacker's own session) and the sole
v1.0 user closes the handle immediately after attach.

## Disposition

Fixed: `srvconn_is_kernel_attached(cn) → −1` added to both CSRVCLIENT
branches, mirroring the (now-retired) KOBJ_SRV arms' guard. The
prosecutor found this one and the self-audit missed it — the
two-prosecutor cross-coverage lesson. Regression: control I/O works
pre-attach, refused post-attach; pre-fix it drained the staged bytes.
The guard-must-follow-the-endpoint principle is now on
[[sub-kernel-devsrv]]'s Prosecution list.
