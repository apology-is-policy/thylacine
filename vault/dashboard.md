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
| [[arc-go-build]] | active | 9 |
| [[arc-go-ide]] | active | 2 |
| [[arc-identity-detour]] | active | 5 |
| [[arc-net]] | active | 3 |
| [[arc-pouch-boot]] | active | 1 |
| [[arc-vault]] | active | 4 |

## Open seams: 10

- [[seam-221-idle-pump-wake]] (sub-kernel-ninep-dev9p-poll)
- [[seam-223-pump-tail-starvation]] (sub-kernel-ninep-dev9p-poll)
- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-848-pivot-walk-race]] (sub-kernel-ninep-attach)
- [[seam-90-hung-server]] (sub-kernel-ninep-client)
- [[seam-co-fidless-wstat]] (sub-kernel-ninep-dev9p)
- [[seam-wb-close-flush-slot]] (sub-kernel-ninep-dev9p)

## Recent changes

- 2026-07-31 [[chg-2026-07-31-ninep-area-sweep]] — The 9P-area sweep: wire, session, transports, attach, dev9p, dev9p.poll
- 2026-07-31 [[chg-2026-07-31-ninep-pilot]] — The 9P-client pilot: one subsystem end-to-end across all four planes
- 2026-07-31 [[chg-2026-07-31-quaestor]] — Quaestor: the Go vault registrar + MCP layer; lint.py retired
- 2026-07-31 [[chg-2026-07-31-vault-commit-0]] — Commit 0: the vault schema, linter, spine, and views
- 2026-07-19 [[chg-2026-07-19-90-death-block-through]] — #90: frame-atomic reader-recv DEATH block-through (spec-first)
- 2026-07-19 [[chg-2026-07-19-99-create-errno]] — #99: propagate the real create errno + drop the stale negative dentry on EEXIST
- 2026-07-17 [[chg-2026-07-17-8c3-reader-role]] — 8c-3 (#89): frame-atomic release of the reader role across a debug stop
- 2026-07-13 [[chg-2026-07-13-375-spill]] — #375: spill -- out_buf never re-read after the park drops c->lock
<!-- generated:end -->
