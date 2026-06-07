# TEST-PLAN: cmp (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`cmp FILE1 FILE2`. Byte-compares two files. Exit 0 = identical, 1 = differ,
2 = error. The "differ" message goes to stdout (POSIX); "EOF on" to stderr.

Preconditions: `/A`=`abc\ndef\n`; `/B`=`abc\ndef\n` (== A); `/C`=`abc\nxef\n`
(differs from A at byte 5, line 2); `/D`=`abc\n` (prefix of A).

| # | argv | output | exit |
|---|---|---|---|
| T1 | `["cmp","/A","/B"]` | (none) | 0 |
| T2 | `["cmp","/A","/C"]` | (stdout) `/A /C differ: byte 5, line 2\n` | 1 |
| T3 | `["cmp","/A","/D"]` | (stderr) `cmp: EOF on /D\n` | 1 |
| T4 | `["cmp","/A"]` (missing 2nd) | (stderr) `cmp: missing operand` | 2 |
| T5 | `["cmp","/missing","/A"]` | (stderr) open error | 2 |
| T6 | `["cmp","/A","/A"]` | (none) (same file) | 0 |
