# TEST-PLAN: tr (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`tr SET1 [SET2]` translate; `tr -d SET1` delete. SETs expand `a-z` byte
ranges. Reads stdin, writes stdout. No -s/-c at v1.

| # | argv (+ stdin) | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["tr","a-z","A-Z"]`, `hello\n` | `HELLO\n` | 0 |
| T2 | `["tr","-d","aeiou"]`, `hello\n` | `hll\n` | 0 |
| T3 | `["tr","abc","x"]`, `cab\n` | `xxx\n` (SET2 last byte repeats) | 0 |
| T4 | `["tr","-d","0-9"]`, `a1b2c3\n` | `abc\n` | 0 |
| T5 | `["tr","a-z"]` (translate, 1 set) | (stderr) needs two sets | 1 |
| T6 | `["tr"]` | (stderr) missing operand | 1 |
