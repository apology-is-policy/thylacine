# TEST-PLAN: mkdir (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Needs a WRITABLE filesystem (dev9p mount; the boot ramfs is read-only).

`mkdir [-p] DIR...`. Absolute paths only (G07).

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["mkdir","/w/new"]` | creates `/w/new` (mode 0755) | 0 |
| T2 | `["mkdir","/w/a/b/c"]` (a,b absent) | fails (parent missing) | 1 |
| T3 | `["mkdir","-p","/w/a/b/c"]` | creates a, a/b, a/b/c | 0 |
| T4 | `["mkdir","/w/exists"]` (exists) | error (EEXIST) | 1 |
| T5 | `["mkdir","-p","/w/exists"]` | no error (idempotent) | 0 |
| T6 | `["mkdir","/ro/x"]` (read-only fs) | error | 1 |
| T7 | `["mkdir"]` | (stderr) missing operand | 1 |
