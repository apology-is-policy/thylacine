# TEST-PLAN: realpath (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`realpath PATH...`. LEXICAL only (G11 + G07): no symlink resolution (no
readlink surface), no existence check, no relative-path support (no cwd).
Collapses `.`, `..`, `//`.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["realpath","/a/b/../c"]` | `/a/c\n` | 0 |
| T2 | `["realpath","/a/./b/"]` | `/a/b\n` | 0 |
| T3 | `["realpath","/a//b"]` | `/a/b\n` | 0 |
| T4 | `["realpath","/"]` | `/\n` | 0 |
| T5 | `["realpath","/.."]` | `/\n` (pop past root stays root) | 0 |
| T6 | `["realpath","rel/path"]` | (stderr) relative unsupported (no cwd) | 1 |
| T7 | `["realpath"]` | (stderr) missing operand | 1 |

Diverges from GNU realpath: it does NOT resolve symlinks or require the
path to exist (closer to `realpath -m -s`). Documented in G11.
