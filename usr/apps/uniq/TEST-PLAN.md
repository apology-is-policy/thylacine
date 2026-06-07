# TEST-PLAN: uniq (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`uniq [-c] [FILE]` -- collapse ADJACENT duplicate lines (-c prefixes the
run count). Reads one FILE or stdin.

Precondition: `/U` contains `a\na\nb\na\n` (a,a,b,a -> adjacent: a x2, b, a).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["uniq","/U"]` | `a\nb\na\n` | 0 |
| T2 | `["uniq","-c","/U"]` | `      2 a\n      1 b\n      1 a\n` | 0 |
| T3 | `["uniq"]`, stdin = `/U` bytes | `a\nb\na\n` | 0 |
| T4 | `["uniq","/missing"]` | (stderr) error | 1 |

Only ADJACENT duplicates merge (GNU semantics); `sort | uniq` dedupes
globally. A trailing newline does not create a phantom empty line.
