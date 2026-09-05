---
id: fnd-stalk3a-r1-f2
type: fnd
title: "Raw SrvService pointers into entries[] carry no registry ref — a MORTAL registry's last unref would dangle them"
round: adt-stalk3a-r1
severity: P3
status: deferred
surface: [sub-kernel-devsrv]
threatens: [inv-i1]
seam: seam-srv-registry-lifecycle
created: 2026-07-31
---
## Prosecution

`srv_registry_unref`'s last drop kfrees the registry INCLUDING
`entries[]`. A KObj_Srv listener handle's obj, and any in-flight
`devsrv_open_connect` / `svc_listener_poll`, hold a raw `SrvService *`
into that array with NO registry ref. Unfireable while every such
pointer targets the immortal boot registry — but the moment a MORTAL
per-session registry exists, its last unref racing a live listener
handle is a UAF into freed registry storage.

## Disposition

Deferred as the standing MORTAL-REGISTRY ORDERING OBLIGATION (a forward
note lives at `srv_registry_unref`): any future non-immortal registry
must order its last unref AFTER every listener/connection handle into it
closes (the session poster group-terminates first, #811), OR those
holders must carry covering registry refs. Dormant by construction at
v1.0 (the per-session registry was never built — every session shares
the boot registry); discharges with [[seam-srv-registry-lifecycle]].
