---
id: view-closed-sub-kernel-devsrv
type: view
title: "Do-not-re-report preamble — sub-kernel-devsrv"
query: closed:sub-kernel-devsrv
---
# Do-not-re-report preamble — sub-kernel-devsrv

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-devsrv`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
16 closed findings on [[sub-kernel-devsrv]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-957-r1-f1]] [P1] Single-hop open of a /srv leaf leaked the connection endpoint (fixed) — Fixed in-commit by adopting open()'s return exactly like stalk's
- [[fnd-p5srv-r1-f1]] [P1] Production /srv ops never armed client_deadline_ns — a hung server wedged its caller indefinitely (fixed) — Fixed in the audit-close commit: `srvconn_set_client_deadline` armed
- [[fnd-p5srv-r1-f14]] [P3] Claimed missing poller wake on the connect failure paths (withdrawn) — WITHDRAWN: both failure paths are correct — the early-bail path frees
- [[fnd-p5srv-r1-f2]] [P2] SrvService.magic + devsrv_svc_ref.magic offsets unpinned — the first-u64 discriminator rested on field order (fixed) — Fixed: both `_Static_assert`s added to `devsrv.h`, mirroring srvconn's.
- [[fnd-p5srv-r1-f6]] [P2] The post/connect handlers skipped sys_validate_user_buf before their per-byte copy loops (fixed) — Fixed: both handlers pre-validate the range before the copy loops.
- [[fnd-p5srv-r1-f7]] [P3] Claimed name_va buffer overrun in the post handler (withdrawn) — WITHDRAWN by the prosecutor's own self-check:
- [[fnd-p5srv-r1-f8]] [P3] A burst of hung handshakes can transiently exhaust SRV_MAX_CONNS (documented) — Documented, no code: with the F1 deadline fix even a hung handshake
- [[fnd-rw4-rev2-f2]] [P3] RW-4 R2-F2: stat_native-implies-seekable regressed lseek on devsrv/devproc (fixed) — Fixed: an explicit `dev->seekable` flag (true on devramfs + dev9p
- [[fnd-stalk3a-r1-f1]] [P3] devsrv roots carried no per-instance devno — every registry root had mount-key identity (s,0,0) (fixed) — Fixed in the audit close: `c->devno = spoor_next_devno()` per attach
- [[fnd-stalk3a-r1-f3]] [P3] A devsrv-root clone's aux is UNOWNED until devsrv.walk takes ownership — a bare clone-then-clunk phantom-unrefs (documented) — Documented as the clone contract at `devsrv_attach_registry`'s
- [[fnd-stalk3a-r1-f4]] [P3] The per-Proc srv_conn_count decrement must move with the open=connect migration (fixed) — Closed by stalk-3b-D taking the OTHER exit: the per-Proc cap (and the
- [[fnd-stalk3b-r1-f1]] [P2] The kernel_attached I/O guard did not follow the conn endpoint from KObj_Srv to KOBJ_SPOOR (fixed) — Fixed: `srvconn_is_kernel_attached(cn) → −1` added to both CSRVCLIENT
- [[fnd-stalk3b-r1-f3]] [P3] With the per-Proc cap removed, one Proc can hold all 64 global conn slots (documented) — Documented as an ACCEPTED cross-Proc fairness tradeoff — memory stays
- [[fnd-stalk3c-r1-f1]] [P3] handle.c's KOBJ_SRV release comment cited the deleted connect core; the SrvConn arm went defensively dead (fixed) — Fixed: comment reworded to the as-built; the `SRV_CONN_MAGIC` arm
- [[fnd-stalk3c-r1-f2]] [P3] Residual stale references to the retired /srv symbols across seven files' comments (fixed) — Fixed: all reworded to create=post / open=connect /
- [[fnd-stalk3c-r1-f3]] [P3] The boot-registry getter comment overstated production reachability — I-1 prosecuted directly and HELD (fixed) — The prosecution came back HELD and STRENGTHENED: post/connect resolve
<!-- generated:end -->
