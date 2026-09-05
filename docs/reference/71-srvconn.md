# 71 — srvconn: the `/srv` per-connection transport [ABSORBED INTO THE VAULT]

This document was absorbed at the srv-area sweep
(`chg-2026-07-31-srv-sweep`). Its content now lives, code-verified and
current, in the dossier:

    vault/system/kernel/srv/sub-kernel-srvconn.md

(the rings, the blocking flow control, the #354 role park, teardown,
the identity fields — with the staleness this file had accreted
corrected: the embedded per-conn `p9_client` / `recv_buf` /
`client_fid` were retired at stalk-3b-beta-D, the "no-writer-block
design" caveat was retired by #348/#349/CF-3 B, and the ring sizes are
per-class heap allocations, not the inline figures below). The audit
history (P5-corvus-srv-impl, #348, CF-3 B) lives as adt-/fnd- Record
notes; the do-not-re-report preamble is
vault/views/view-closed-sub-kernel-srvconn.md.

The dossier supersedes this file. Do not extend this stub -- extend the
dossier and its linked registry notes. This stub is deleted after the
vault migration's full-corpus verification pass (vault/meta/schema.md
section 10.6).
