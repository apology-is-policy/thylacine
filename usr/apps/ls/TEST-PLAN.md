# TEST-PLAN: ls (A2)

Status: authored, NOT executed. Verified: cargo build + clippy clean.
Read-only; works on the boot ramfs.

`ls [-la1] [DIR...]`. One name per line (no column packing at v1). With no
operand, lists "/" (no cwd, G07). -a includes dotfiles + . / ..; -l long.

| # | argv | fd-1 output | exit |
|---|---|---|---|
| T1 | `["ls"]` | sorted non-hidden names under `/`, one per line | 0 |
| T2 | `["ls","/DIR"]` | sorted names under /DIR | 0 |
| T3 | `["ls","-a","/DIR"]` | includes `.`, `..`, dotfiles | 0 |
| T4 | `["ls","-l","/DIR"]` | `<mode> <links> <uid> <gid> <size> <name>` per entry | 0 |
| T5 | `["ls","-la","/DIR"]` | long format incl. hidden | 0 |
| T6 | `["ls","/A","/B"]` | `/A:` block, blank, `/B:` block | 0 |
| T7 | `["ls","/missing"]` | (stderr) error | 1 |
| T8 | `["ls","-Z","/"]` | (stderr) invalid option | 1 |

Notes: -l derives the mode string from Metadata.permissions() + the type
bits; the readdir type-byte ambiguity (G10) is handled in is_dir(). The
readdir entry order is unspecified, hence the explicit sort.
