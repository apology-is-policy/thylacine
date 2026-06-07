# TEST-PLAN: cut (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`cut -f LIST [-d DELIM] [FILE...]` (fields, default delim TAB) or
`cut -c LIST [FILE...]` (byte positions). LIST = comma-separated single
positions and N-M / N- / -M ranges.

Precondition: `/C` = `a:b:c:d\n1:2:3:4\n` (colon-separated).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["cut","-d",":","-f","1,3","/C"]` | `a:c\n1:3\n` | 0 |
| T2 | `["cut","-d:","-f2-","/C"]` | `b:c:d\n2:3:4\n` | 0 |
| T3 | `["cut","-c","1-3","/C"]` | `a:b\n1:2\n` (first 3 bytes) | 0 |
| T4 | `["cut","-d",":","-f","9","/C"]` | empty lines (no field 9) | 0 |
| T5 | `["cut","-f1","no-delim-line"]` | whole line unchanged (no delim) | 0 |
| T6 | `["cut","-f","x","/C"]` | (stderr) invalid list | 1 |
| T7 | `["cut","/C"]` (neither -f nor -c) | (stderr) specify -f or -c | 1 |

Notes: -d takes the FIRST byte of its argument as the delimiter. A line
with no delimiter is passed through unchanged (GNU -f behavior).
