# TEST-PLAN: tail (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`tail [-n N | -N] [FILE...]`. Last N lines (default 10). Reads each input
fully, then scans backward. A single trailing newline is not a phantom line.

Preconditions: `/T` contains `L1\n..L20\n` (20 lines); `/Tn` contains
`a\nb\nc` (3 lines, NO trailing newline).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["tail","/T"]` | `L11\n..L20\n` (last 10) | 0 |
| T2 | `["tail","-n","2","/T"]` | `L19\nL20\n` | 0 |
| T3 | `["tail","-3","/T"]` | `L18\nL19\nL20\n` | 0 |
| T4 | `["tail","-n","2","/Tn"]` | `b\nc` (no trailing newline) | 0 |
| T5 | `["tail","-n","0","/T"]` | (empty) | 0 |
| T6 | `["tail","-n","100","/T"]` | all 20 lines | 0 |
| T7 | `["tail","/T","/T"]` | banner + last-10, blank, banner, last-10 | 0 |
| T8 | `["tail"]`, fd 0 piped | last 10 lines of stdin | 0 |
| T9 | `["tail","/missing"]` | (stderr) error | 1 |
