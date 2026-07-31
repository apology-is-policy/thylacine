---
id: fnd-stalk3b-r1-f2
type: fnd
title: "The SYS_ATTACH_9P_SRV ABI doc still described the retired KObj_Srv / embedded-client path"
round: adt-stalk3b-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-06-03-stalk3b-open-connect
created: 2026-07-31
---
## Prosecution

`syscall.h`'s `SYS_ATTACH_9P_SRV` contract described the pre-C1 world:
a KObj_Srv source handle, the embedded per-conn 9P client, the
SYS_SRV_CONNECT provenance. A maintainer coding against the header
would build the retired shape.

## Disposition

Fixed in the close: the doc rewritten to the as-built — `srv_fd` is a
KOBJ_SPOOR CSRVCLIENT conn Spoor from open=connect; the composition is
`srvconn_attach_dev9p_root` + `srvconn_set_kernel_attached` over the
SHARED kernel client; the failure list corrected.
