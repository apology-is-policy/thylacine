---
id: chg-2026-09-06-fd-syscalls-doc-absorb
type: chg
title: "absorb docs/reference/54-sys-fd-syscalls (SYS_CLOSE/SYS_DUP + pipe-probe): clean redirect to sub-kernel-handle + sub-kernel-syscall-dispatch"
date: 2026-09-06
arc: arc-vault
commits: ["8de23124"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
SYS_CLOSE=11 / SYS_DUP=12 (thin wrappers over handle_close/handle_dup) + the
/pipe-probe integration test. Verified atom-by-atom; sub-kernel-handle (audit:
hard) LAPS the doc.

WHERE EACH ATOM LIVES (verified, not assumed):
- handle_close (release), handle_dup (a second reference with REDUCED rights =
  the RightsCeiling, I-6 -- prose, no note), the dup-variant family, the per-kind
  acquire/release asymmetry (KOBJ_SPOOR spoor_ref/spoor_clunk discriminated by the
  leading u64), handle_close_on_exec -> sub-kernel-handle (:34/40/49-52/63/109).
- The handlers (thin current_thread wrappers + new_rights & ~RIGHT_ALL reject) ->
  sub-kernel-syscall-dispatch.
- The RightsCeiling spec -> specs/handles.tla.

AHEAD: the doc's "No dup2 variant" caveat is STALE -- handle_dup_posix (POSIX
lowest-free-fd, verbatim rights) + handle_dup_to/handle_replace (forced-index) all
exist now, carried by the dossier. The fd surface grew past this first-cut doc.

/pipe-probe: a historical first-witness ("the first empirical test" of the byte-
I/O composition), now witnessed by the whole boot suite; a P5-era probe artifact
like the other *-probe binaries, correctly UNOWNED (grep: 0 vault hits).

Zero-fold. Redirect stub.
