# TEST-PLAN: head (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`head [-n N | -N] [FILE...]`. First N lines (default 10). Streams: stops
after the N-th newline (does not slurp the whole file).

Preconditions: `/H` contains lines `L1\nL2\n...\nL20\n` (20 lines).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["head","/H"]` | `L1\n..L10\n` (first 10) | 0 |
| T2 | `["head","-n","3","/H"]` | `L1\nL2\nL3\n` | 0 |
| T3 | `["head","-5","/H"]` | `L1\n..L5\n` | 0 |
| T4 | `["head","-n","0","/H"]` | (empty) | 0 |
| T5 | `["head","-n","100","/H"]` | all 20 lines | 0 |
| T6 | `["head","/H","/H"]` | `==> /H <==\n`+10 lines, blank, banner, 10 lines | 0 |
| T7 | `["head"]`, fd 0 piped | first 10 lines of stdin | 0 |
| T8 | `["head","-n","x","/H"]` | (stderr) `head: invalid number of lines` | 1 |
| T9 | `["head","/missing"]` | (stderr) error | 1 |

Edge: a file with fewer than N lines emits all of it. A final line without
a trailing newline is still emitted (the loop ends at EOF).
