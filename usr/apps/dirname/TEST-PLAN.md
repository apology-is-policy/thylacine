# TEST-PLAN: dirname (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`dirname PATH`. Exercises libthyla-rs `Path::parent()` + the POSIX mapping
of its None / Some("") results (DOC-GAP G08).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["dirname","/usr/lib"]` | `/usr\n` | 0 |
| T2 | `["dirname","/usr/lib/"]` | `/usr\n` (trailing slash) | 0 |
| T3 | `["dirname","usr/lib"]` | `usr\n` | 0 |
| T4 | `["dirname","usr"]` | `.\n` (relative single -> ".") | 0 |
| T5 | `["dirname","/"]` | `/\n` | 0 |
| T6 | `["dirname",""]` | `.\n` | 0 |
| T7 | `["dirname","."]` | `.\n` | 0 |
| T8 | `["dirname","/usr"]` | `/\n` | 0 |
| T9 | `["dirname"]` (no operand) | (stderr) `dirname: missing operand` | 1 |
