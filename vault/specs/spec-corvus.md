---
id: spec-corvus
type: spec
title: "corvus.tla — /srv connection identity + lifecycle (and the corvus session layer)"
models: [sub-kernel-devsrv]
pins: []
cfgs:
  - "corvus.cfg — clean (PARTIAL-RUN posture: 74M distinct states / diameter 22 explored without violation, then killed at steady-state queue growth; the full clean run is SUSPENDED for corvus per user direction at the P5 close)"
  - "corvus_buggy_post_without_marker.cfg — buggy: counterexample of ServicePosterEverMarked (a post without the joey-stamped MAY_POST_SERVICE gate)"
  - "corvus_buggy_identity_cached_on_fid.cfg — buggy: counterexample of ConnOpIdentityIsKernelTruth (a server caching peer identity on fid state instead of re-reading per op)"
  - "corvus_buggy_dead_proc_stale.cfg — buggy: counterexample of ConnOpPeerWasLive (a dead peer's stale capability snapshot authorizing an op)"
  - "corvus_buggy_unwrap_cross_user.cfg — buggy (session layer): cross-user unwrap"
  - "corvus_buggy_auth_binding_mutate.cfg — buggy (session layer): AUTH binding mutation"
  - "corvus_buggy_admin_without_proc_cap.cfg — buggy (session layer): admin verb without the proc capability"
  - "corvus_buggy_elevate_without_console.cfg — buggy (session layer): elevation without console attachment"
  - "corvus_buggy_transfer_rebind.cfg — buggy (session layer): transfer/rebind of a session binding"
gate: "Re-run the 8 buggy cfgs on any change to a modeled action's implementation site (the connection-layer trio for kernel /srv changes; all 8 for corvus-side changes). The clean cfg is NOT a pre-commit gate — spec-to-code is suspended for corvus (user direction, 2026-05-20; the state space grew ~3x under ConnTeardown + connections_history and the clean run no longer terminates practically)."
created: 2026-07-31
updated: 2026-07-31
---
## Abstraction boundary

`corvus.tla` models **identity and lifecycle a level above the bytes**:
who may post a name, who owns a connection, whose identity a peer query
reports, and what a poster/peer death does to bindings. Deliberately
beneath the model:

- The byte transport entirely ([[sub-kernel-srvconn]] — rings, flow
  control, roles, deadlines are prose + test validated).
- The `RESERVING` transient of the two-phase post — the spec's
  `PostService` is atomic; reserve/commit/abort is the rollback-safe
  implementation of that atom.
- The accept backlog's bounded FIFO (the spec's `SrvAccept` dequeues an
  abstract pending binding).
- `connections` is the LIVE binding set (peer-uniqueness for `SrvBind`);
  `connections_history` is the append-only ledger
  `ConnOpIdentityIsKernelTruth` checks against — added at the P5 close
  (F5) so reconnect-after-teardown is modeled.

Its invariants are CORVUS-DESIGN C-invariants (C-22/C-23 family), not
ARCH §28 rows — hence `pins: []`; the §28 composition on this surface is
[[inv-i1]] (prose) via the registry reachability, not a TLC-checked
property.

## Action ↔ site map (the kernel connection layer)

| Spec action / invariant | Site |
|---|---|
| `MarkMayPost` | `proc_mark_may_post_service` (one-way, never rfork-propagated) |
| `PostService` | `devsrv_post_listener` → `srv_reserve_in` / `srv_commit` / `srv_abort` |
| `ServiceTombstone` | `srv_proc_exit_notify_in`, called from `exits()` |
| `SrvBind` | `devsrv_open_connect` (mint + enqueue) |
| `SrvAccept` | `srv_accept_blocking` / `sys_srv_accept_for_proc` |
| `SrvPeerOp` | `sys_srv_peer_for_proc` (fresh per-op resolve; never fid-cached) |
| `ProcExit` | the poster-exit accept-backlog drain |
| `ConnTeardown` | `srvconn_teardown` reached from every close path |
| `ServicePosterEverMarked` | the `proc_may_post_service` gate in `devsrv_post_listener` |
| `ConnOpIdentityIsKernelTruth` | the by-value `peer_stripes`/`peer_console` read off the SrvConn |
| `ConnOpPeerWasLive` | `proc_peer_snapshot_by_stripes`'s alive gate (caps/identity/flags/pid fail-close to 0) |

The session-layer actions (AUTH/UNWRAP/ADMIN/SESSION) map into corvus
userspace (`usr/corvus/`) — that half of the map lands with the corvus
sweep.
