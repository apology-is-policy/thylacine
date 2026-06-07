# TEST-PLAN: stat (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`stat FILE...` -- print metadata (via libthyla-rs::fs::metadata / SYS_FSTAT).
Read-only; works on the boot ramfs.

| # | argv | fd-1 output (shape) | exit |
|---|---|---|---|
| T1 | `["stat","/FILE"]` | `  File: /FILE` + Size/Blocks/type line + Mode/Links/Uid/Gid line + Access/Modify/Change line | 0 |
| T2 | `["stat","/DIR"]` | same, with `directory` as the type | 0 |
| T3 | `["stat","/A","/B"]` | one block per file | 0 |
| T4 | `["stat","/missing"]` | (stderr) error; nonzero | 1 |
| T5 | `["stat"]` | (stderr) missing operand | 1 |

Fields exercised: len, blocks, blksize, type bits, permissions (octal),
nlink, uid, gid, atime/mtime/ctime seconds.
