# 70 — devsrv: the `/srv` service registry [ABSORBED INTO THE VAULT]

This document was absorbed at the srv-area sweep
(`chg-2026-07-31-srv-sweep`). Its content now lives, code-verified and
current, in the dossier:

    vault/system/kernel/srv/sub-kernel-devsrv.md

(the registry state machine, create=post / open=connect, the accept +
peer syscall layer with the CURRENT 40-byte `srv_peer_info`
[principal_id/primary_gid/flags/pid appended since this file's 24-byte
description], the close discriminator, poll — with this file's stale
caveats corrected: `devsrv_open` is REAL since stalk-3b-beta,
`srv_conn_open_for_proc`/`srv_reserve`/`srv_lookup` and the name-only
syscalls are retired, `proc_flags` writers are atomic since the
multi-thread lift). The audit history (P5-corvus-srv-impl,
stalk-3a/3b/3c) lives as adt-/fnd- Record notes; the open debt as
seam-srv-* notes; the do-not-re-report preamble is
vault/views/view-closed-sub-kernel-devsrv.md.

The dossier supersedes this file. Do not extend this stub -- extend the
dossier and its linked registry notes. This stub is deleted after the
vault migration's full-corpus verification pass (vault/meta/schema.md
section 10.6).
