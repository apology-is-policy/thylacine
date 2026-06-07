# TEST-PLAN: basename (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`basename PATH [SUFFIX]`. Exercises libthyla-rs `Path::file_name()` + the
POSIX recovery for its None cases (DOC-GAP G08).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["basename","/usr/lib"]` | `lib\n` | 0 |
| T2 | `["basename","/usr/lib/"]` | `lib\n` (trailing slash) | 0 |
| T3 | `["basename","usr"]` | `usr\n` | 0 |
| T4 | `["basename","/usr/include/stdio.h",".h"]` | `stdio\n` | 0 |
| T5 | `["basename","/"]` | `/\n` (G08 recovery) | 0 |
| T6 | `["basename","."]` | `.\n` (G08 recovery) | 0 |
| T7 | `["basename",".."]` | `..\n` (G08 recovery) | 0 |
| T8 | `["basename",""]` | `\n` (empty) | 0 |
| T9 | `["basename"]` (no operand) | (stderr) `basename: missing operand` | 1 |
| T10 | `["basename","libfoo.so",".so"]` | `libfoo\n` | 0 |
| T11 | `["basename","x",".x"]` | `x\n` (suffix == whole name not stripped) | 0 |
