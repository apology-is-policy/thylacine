# TEST-PLAN: seq (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`seq [FIRST [INCR]] LAST` -- integer sequence, one per line.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["seq","3"]` | `1\n2\n3\n` | 0 |
| T2 | `["seq","2","5"]` | `2\n3\n4\n5\n` | 0 |
| T3 | `["seq","1","2","9"]` | `1\n3\n5\n7\n9\n` | 0 |
| T4 | `["seq","5","-1","1"]` | `5\n4\n3\n2\n1\n` | 0 |
| T5 | `["seq","3","1"]` | (empty: first > last, incr +1) | 0 |
| T6 | `["seq","1","0","9"]` | (stderr) increment must not be zero | 1 |
| T7 | `["seq","x"]` | (stderr) invalid number | 1 |
| T8 | `["seq"]` | (stderr) usage | 1 |

Integer-only at v1 (no fractional sequences). Iteration is overflow-safe.
