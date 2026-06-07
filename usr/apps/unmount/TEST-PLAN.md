# TEST-PLAN: unmount (A4)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`unmount MOUNTPOINT...` -- remove namespace mounts from this Proc's
territory. AUDIT-BEARING (territory unmount). Same namespace-locality caveat
as bind: only this Proc's namespace is affected.

| # | argv | expected | exit |
|---|---|---|---|
| T1 | `["unmount","/b"]` after a `bind /a /b` (same Proc) | `/b` reverts to its prior contents | 0 |
| T2 | `["unmount","/never-mounted"]` | error (nothing mounted there) | 1 |
| T3 | `["unmount","/a","/b"]` | unmounts both | 0 |
| T4 | `["unmount"]` | (stderr) missing operand | 1 |

Pair with bind in one inheriting namespace to observe the round trip.
