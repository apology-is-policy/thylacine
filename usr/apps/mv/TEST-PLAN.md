# TEST-PLAN: mv (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Needs a writable filesystem.

`mv SRC... DST`. Tries atomic rename; on cross-session failure, falls back
to copy+unlink FOR FILES (cross-session directory move is a v1.0 limitation).

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["mv","/w/a","/w/b"]` (same session) | renames a->b atomically | 0 |
| T2 | `["mv","/w/a","/w/dir"]` (dir dst) | moves a into dir | 0 |
| T3 | `["mv","/w/a","/w/b","/w/dir"]` | moves both into dir | 0 |
| T4 | `["mv","/w/a","/w/b","/w/file"]` (not dir) | error: not a directory | 1 |
| T5 | `["mv","/s1/file","/s2/file"]` (cross-session) | copy+unlink fallback | 0 |
| T6 | `["mv","/s1/dir","/s2/dir"]` (cross-session dir) | error (v1 limitation) | 1 |
| T7 | `["mv","/w/missing","/w/x"]` | error | 1 |
| T8 | `["mv","/only-one"]` | (stderr) missing operand | 1 |
