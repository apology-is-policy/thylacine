# TEST-PLAN: sort (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`sort [-rnu] [FILE...]` -- lexical (or -n numeric) sort; -r reverse, -u drop
adjacent-after-sort duplicates. Reads all inputs into memory.

Precondition: `/S` = `banana\napple\ncherry\n`; `/N` = `10\n2\n33\n4\n`.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["sort","/S"]` | `apple\nbanana\ncherry\n` | 0 |
| T2 | `["sort","-r","/S"]` | `cherry\nbanana\napple\n` | 0 |
| T3 | `["sort","-n","/N"]` | `2\n4\n10\n33\n` | 0 |
| T4 | `["sort","/N"]` (lexical) | `10\n2\n33\n4\n` | 0 |
| T5 | `["sort","-u",dups]` | sorted, adjacent dups removed | 0 |
| T6 | `["sort"]`, stdin piped | sorted stdin | 0 |
| T7 | `["sort","/missing"]` | (stderr) error | 1 |
