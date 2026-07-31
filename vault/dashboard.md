---
id: dashboard
type: view
title: "Dashboard"
query: dashboard
---
# Dashboard

Generated — do not edit between the markers (`meta/lint.py --render`).

<!-- generated:begin -->
## Arcs

| arc | status | chunks |
|---|---|---|
| [[arc-go-build]] | active | 3 |
| [[arc-go-ide]] | active | 2 |
| [[arc-identity-detour]] | active | 2 |
| [[arc-vault]] | active | 2 |

## Open seams: 5

- [[seam-350-async-eagain]] (sub-kernel-ninep-client)
- [[seam-56-netd-cancelled-tag]] (sub-kernel-ninep-client)
- [[seam-841-mi-harness]] (sub-kernel-ninep-client)
- [[seam-845-untrusted-server]] (sub-kernel-ninep-client)
- [[seam-90-hung-server]] (sub-kernel-ninep-client)

## Recent changes

- 2026-07-31 [[chg-2026-07-31-ninep-pilot]] — The 9P-client pilot: one subsystem end-to-end across all four planes
- 2026-07-31 [[chg-2026-07-31-vault-commit-0]] — Commit 0: the vault schema, linter, spine, and views
- 2026-07-19 [[chg-2026-07-19-90-death-block-through]] — #90: frame-atomic reader-recv DEATH block-through (spec-first)
- 2026-07-17 [[chg-2026-07-17-8c3-reader-role]] — 8c-3 (#89): frame-atomic release of the reader role across a debug stop
- 2026-07-13 [[chg-2026-07-13-375-spill]] — #375: spill -- out_buf never re-read after the park drops c->lock
- 2026-07-13 [[chg-2026-07-13-5253-send-dispositions]] — #52/#53: never-sent tag reclaim + flush-EAGAIN rollback
- 2026-06-24 [[chg-2026-06-24-349-flow-control]] — #349: c2s back-pressure is flow control, not session death
- 2026-06-04 [[chg-2026-06-04-845-tflush]] — #845: Tflush-on-abandon closes the DIED-leaked outstanding slot
<!-- generated:end -->
