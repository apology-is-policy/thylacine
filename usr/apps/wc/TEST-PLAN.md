# TEST-PLAN: wc (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`wc [-clwm] [FILE...]`. Default columns: lines, words, bytes (7-wide each),
then name. `-m` (chars) is approximated as bytes at v1 (no multibyte).

Preconditions: `/W` is a file containing `one two\nthree\n` (2 lines, 3
words, 14 bytes).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["wc","/W"]` | `      2      3     14 /W\n` | 0 |
| T2 | `["wc","-l","/W"]` | `      2 /W\n` | 0 |
| T3 | `["wc","-w","/W"]` | `      3 /W\n` | 0 |
| T4 | `["wc","-c","/W"]` | `     14 /W\n` | 0 |
| T5 | `["wc","-lw","/W"]` | `      2      3 /W\n` | 0 |
| T6 | `["wc","/W","/W"]` | two lines + `     ... total\n` | 0 |
| T7 | `["wc"]`, fd 0 piped `a b\n` | `      1      2      4\n` (no name) | 0 |
| T8 | `["wc","-z","/W"]` | (stderr) `wc: invalid option -- 'z'` | 1 |
| T9 | `["wc","/missing"]` | (stderr) error | 1 |

Edge: a file with no trailing newline counts lines as the number of `\n`
bytes (GNU-compatible: a final unterminated line is not counted as a line).
