# TEST-PLAN: touch (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Needs a writable filesystem.

`touch [-c] FILE...`. PARTIAL (DOC-GAP G12): creates absent files empty;
CANNOT update the mtime of an existing file (no atime/mtime bits in
t_wstat) -- existing files are a no-op.

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["touch","/w/new"]` (absent) | creates empty `/w/new` | 0 |
| T2 | `["touch","/w/exists"]` | NO-OP (mtime bump unsupported, G12) | 0 |
| T3 | `["touch","-c","/w/absent"]` | no-create: does nothing | 0 |
| T4 | `["touch","/ro/x"]` (read-only) | error on create | 1 |
| T5 | `["touch"]` | (stderr) missing operand | 1 |

Note: T2's no-op is a v1.0 limitation; on GNU it would refresh mtime.
