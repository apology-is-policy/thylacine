# TEST-PLAN: rmdir (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Needs a writable filesystem.

`rmdir DIR...` -- remove EMPTY directories.

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["rmdir","/w/empty"]` | removes the empty dir | 0 |
| T2 | `["rmdir","/w/nonempty"]` | error (not empty) | 1 |
| T3 | `["rmdir","/w/missing"]` | error (not found) | 1 |
| T4 | `["rmdir","/w/a","/w/b"]` (both empty) | removes both | 0 |
| T5 | `["rmdir"]` | (stderr) missing operand | 1 |
