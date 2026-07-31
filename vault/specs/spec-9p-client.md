---
id: spec-9p-client
type: spec
title: "9p_client.tla"
models: [sub-kernel-ninep-client]
pins: [inv-i10, inv-i11]
cfgs:
  - "9p_client.cfg -- clean (TagAndOpAccounting + FidStability + BoundedOutstanding; 462/197 states, depth 9)"
  - "9p_client_buggy_tag_collision.cfg -- buggy: counterexample of TagAndOpAccounting (alloc_tag returns an in-use tag)"
  - "9p_client_buggy_fid_after_clunk.cfg -- buggy: counterexample of FidStability / I-11 (IO on a fid after Tclunk)"
  - "9p_client_buggy_ooo_match.cfg -- buggy: counterexample of TagAndOpAccounting (Rmsg paired with the wrong outstanding op)"
  - "9p_client_buggy_unbounded.cfg -- buggy: counterexample of BoundedOutstanding (send past MaxWindow, no back-pressure)"
  - "9p_client_buggy_async_clunk_tag_leak.cfg -- buggy: counterexample of TagAndOpAccounting (ownerless Rclunk consumed without freeing the tag -- a permanently burned outstanding slot)"
gate: "Pre-commit buggy-cfg re-run for ANY change to tag/fid/outstanding semantics on the 9P surfaces (the standing spec-suspension terms keep existing-spec buggy cfgs as gates)."
created: 2026-07-31
updated: 2026-07-31
---
## Abstraction

One 9P session; op kinds collapsed to `{walk, clunk, io}` (the invariants do
not depend on per-op semantics — the whole 9P2000.L family plus Stratum
extensions ride the `io` kind). Deliberately beneath the model:

- **Flow control** (#349): back-pressure appears only as `BoundedOutstanding`
  send-precondition refusal; the EAGAIN/spill/self-pump/park machinery is not
  modeled (prose + audit + [[gate-smp]]).
- **The reader election** (#841): who drains the wire is below the
  abstraction — `ReceiveOp(t)` IS the ownerless drain, whoever performs it.
- **Cross-session isolation**: the server's responsibility (Stratum's own
  specs).
- The impl's monotonic never-reused fid allocator makes the spec's
  finite-FidIds reuse traces a SUPERSET of the impl's.

## Action-site map

Condensed from `specs/SPEC-TO-CODE.md::9p_client.tla` (authoritative until
absorbed; its per-row line numbers pre-date the #841 restructure):

| Spec action | Impl |
|---|---|
| `OpenSession` | `p9_session_send_version` + `p9_session_send_attach` + `p9_session_dispatch_rmsg` (Rversion → VERSIONED; Rattach → OPEN + bind root_fid) |
| `SendWalk(t, src, new)` | `p9_session_send_walk` (preconditions: src bound, new not bound, no in-flight op on new) |
| `SendClunk(t, fid)` | `p9_session_send_clunk` — send-time unbind BEFORE storing the outstanding entry (I-11). The async clunk (`p9_client_clunk_async`) left the clean model UNCHANGED: SendClunk already unbinds at send + holds the tag until `ReceiveOp(t)`; the ownerless drain is who-agnostic |
| `SendIO(t, fid)` | the `p9_session_send_*` op family (lopen/lcreate/read/write/getattr/setattr/readdir/statfs/fsync/mutations/weft) |
| `ReceiveOp(t)` | `p9_session_dispatch_rmsg` — tag-indexed pairing; Rlerror generic; type-mismatch rejected |
| `CloseSession` | `p9_session_close` (refuses while in-flight ≠ {}) |

Invariant → enforcement: `TagAndOpAccounting` → alloc_tag/clear_outstanding +
the `awaiting_flush`/`abandoned` retirement discipline ([[inv-i10]]);
`FidStability` → send-time unbind + monotonic fids ([[inv-i11]]);
`BoundedOutstanding` → the 64-wide outstanding table as the window.
