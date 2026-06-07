# TEST-PLAN: cp (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Source can be read-only ramfs; destination needs a writable filesystem.

`cp [-r] SRC... DST`. Reads via File, writes via aux-rt::fs::create. Does
NOT preserve mode/owner/timestamps (G12).

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["cp","/src","/w/dst"]` | `/w/dst` == bytes of `/src` | 0 |
| T2 | `["cp","/src","/w/dir"]` (dir) | creates `/w/dir/src` | 0 |
| T3 | `["cp","/a","/b","/w/dir"]` | copies both into the dir | 0 |
| T4 | `["cp","/a","/b","/w/file"]` (not dir) | error: target not a directory | 1 |
| T5 | `["cp","/srcdir","/w/dst"]` (no -r) | error: omitting directory | 1 |
| T6 | `["cp","-r","/srcdir","/w/dst"]` | recursively copies the tree | 0 |
| T7 | `["cp","/missing","/w/x"]` | error (not found) | 1 |
| T8 | `["cp","/only-one"]` | (stderr) missing file operand | 1 |
