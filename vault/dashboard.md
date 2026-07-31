---
id: dashboard
type: view
title: "Dashboard"
query: dashboard
---
# Dashboard

Generated — do not edit between the markers (`quaestor render`).

<!-- generated:begin -->
## Arcs

| arc | status | chunks |
|---|---|---|
| [[arc-corvus-srv]] | active | 1 |
| [[arc-go-build]] | active | 20 |
| [[arc-go-ide]] | active | 2 |
| [[arc-identity-detour]] | active | 8 |
| [[arc-net]] | active | 12 |
| [[arc-pouch-boot]] | active | 1 |
| [[arc-vault]] | active | 6 |
| [[arc-weft]] | active | 4 |

## Open seams: 22

- [[seam-220-netd-listener-poll]] (sub-netd-server)
- [[seam-221-idle-pump-wake]] (sub-kernel-ninep-dev9p-poll)
- [[seam-223-pump-tail-starvation]] (sub-kernel-ninep-dev9p-poll)
- [[seam-240-lo-redial]] (sub-netd-server)
- [[seam-242-selftest-nonfatal]] (sub-netd-nic)
- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-848-pivot-walk-race]] (sub-kernel-ninep-attach)
- [[seam-90-hung-server]] (sub-kernel-ninep-client)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-larder-cacheable-proxy]] (sub-kernel-larder, sub-kernel-ninep-dev9p)
- [[seam-larder-lazy-array-robustness]] (sub-kernel-larder)
- [[seam-larder-loom-bypass]] (sub-kernel-larder)
- [[seam-larder-reused-dir-dentries]] (sub-kernel-larder)
- [[seam-larder-shrinker]] (sub-kernel-larder)
- [[seam-larder-stale-child-attr]] (sub-kernel-larder)
- [[seam-netd-host-tests]] (sub-netd-server, sub-netd-nic)
- [[seam-srv-9p-connect-unit]] (sub-kernel-devsrv)
- [[seam-srv-registry-lifecycle]] (sub-kernel-devsrv)
- [[seam-wb-close-flush-slot]] (sub-kernel-ninep-dev9p)

## Recent changes

- 2026-07-31 [[chg-2026-07-31-larder-sweep]] — The Larder sweep: the guest-FS-cache mechanism dossier + the full L1/B1/D44/term audit backfill
- 2026-07-31 [[chg-2026-07-31-netd-sweep]] — The netd sweep: the first userspace area — nic + server dossiers + the net/weft audit backfill
- 2026-07-31 [[chg-2026-07-31-ninep-area-sweep]] — The 9P-area sweep: wire, session, transports, attach, dev9p, dev9p.poll
- 2026-07-31 [[chg-2026-07-31-ninep-pilot]] — The 9P-client pilot: one subsystem end-to-end across all four planes
- 2026-07-31 [[chg-2026-07-31-quaestor]] — Quaestor: the Go vault registrar + MCP layer; lint.py retired
- 2026-07-31 [[chg-2026-07-31-srv-sweep]] — The srv-area sweep: srvconn + devsrv dossiers + the P5/stalk-3/#348/CF-3B audit backfill
- 2026-07-31 [[chg-2026-07-31-vault-commit-0]] — Commit 0: the vault schema, linter, spine, and views
- 2026-07-22 [[chg-2026-07-22-52-nonblock]] — #52 (TyrQuake multiplayer): the BSD nonblocking-socket surface — the netd nonblock verb + E_AGAIN
<!-- generated:end -->
