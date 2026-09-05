---
id: fnd-stalk3c-r1-f2
type: fnd
title: "Residual stale references to the retired /srv symbols across seven files' comments"
round: adt-stalk3c-r1
severity: P3
status: fixed
surface: [sub-kernel-srvconn, sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-03-stalk3c-retire
created: 2026-07-31
---
## Prosecution

After the −711-line retirement, comments in `syscall.c` (the resolver
header — SELF-FOUND; the prosecutor confirmed the code sound but missed
the header), `srvconn.h` (the deadline caller-path, the byte_mode field,
the setter contract), `devsrv.c`/`devsrv.h` (the boot-registry getter,
the ordering note, the accept/poll producer comments), `joey.c`,
`ninep.rs`, and one test label still cited
`srv_conn_open_for_proc`/`SYS_SRV_CONNECT`/`srv_lookup` — zero LIVE
callers (grep-verified both sides), but seven files of comments teaching
the retired shape.

## Disposition

Fixed: all reworded to create=post / open=connect /
`srvconn_attach_dev9p_root`. No code change. The lesson is the F2 sweep
discipline itself: an ABI retirement's comment sweep is part of the
retirement, not a follow-up.
