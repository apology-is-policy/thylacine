---
id: chg-2026-07-08-cf3b-bulk-ring
type: chg
title: "CF-3 B: per-service bulk rings (128 KiB msize) + the #354 role park"
date: 2026-07-08
arc: arc-go-build
commits: ["3b7d1720"]
touched:
  - sub-kernel-srvconn
  - sub-kernel-devsrv
  - sub-kernel-ninep-attach
  - sub-kernel-ninep-client
established: []
closed: [fnd-cf3b-r1-f1, fnd-cf3b-r1-f2, fnd-cf3b-self-freeb, fnd-348-r1-f1]
opened: []
mirrors-checked: []
depth: skeletal
---
The per-service ring-class opt-in (`DMSRVBULK` → `SrvService.ring_msize`
→ heap per-conn rings at 2× class, the inline 64 KiB array retired) so a
bulk FS service negotiates a 128 KiB msize (= `SYS_RW_MAX`: one max
byte-I/O syscall, one RPC) — 4× the per-op payload on top of CF-3 A.
Class is rebind-identity; two-point policy only. Plus the **#354 role
park**: `reading`/`writing` become parkable ROLES
(`chan_role_acquire/release` + per-chan `role_waiters`) — a 2nd
concurrent blocking party parks instead of being refused −1, retiring
the [[fnd-348-r1-f1]] latent and the cross-project reliance on
stratumd's `write_mu` — and the family's THIRD producer goes blocking
(`srvconn_client_send_blocking`, the byte-mode client write). The
9P-client side gains the two-tier out_buf; 9p_attach proposes the conn
class as msize. Audit [[adt-cf3b-r1]] (Fable-5-max) 0/1/0/1 NOT dirty —
both findings fixed in-commit; the pre-audit freeb wedge
([[fnd-cf3b-self-freeb]]) caught by ground truth before landing.
