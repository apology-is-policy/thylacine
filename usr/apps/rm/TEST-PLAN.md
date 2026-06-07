# TEST-PLAN: rm (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Needs a writable filesystem.

`rm [-rRf] FILE...`. -r recurse, -f ignore-missing + suppress errors.

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["rm","/w/file"]` | removes the file | 0 |
| T2 | `["rm","/w/dir"]` (a dir) | error: Is a directory | 1 |
| T3 | `["rm","-r","/w/dir"]` | removes the tree depth-first | 0 |
| T4 | `["rm","/w/missing"]` | error (not found) | 1 |
| T5 | `["rm","-f","/w/missing"]` | ignored (no error) | 0 |
| T6 | `["rm","-rf","/w/tree"]` | removes tree, no errors | 0 |
| T7 | `["rm","-z","/w/x"]` | (stderr) invalid option | 1 |
| T8 | `["rm"]` (no -f) | (stderr) missing operand | 1 |

Edge: a partially-failing recursive rm (e.g. an unremovable child) leaves
the parent dir and reports exit 1.
