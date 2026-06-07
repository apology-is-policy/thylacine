# TEST-PLAN: sleep (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`sleep DURATION...` -- blocks for the SUM of the durations (number + optional
unit s/m/h/d, default s, fractional allowed). Built on time::sleep
(SYS_TORPOR_WAIT timeout); loops in 1-hour chunks past the kernel cap.

| # | argv | effect | exit |
|---|---|---|---|
| T1 | `["sleep","1"]` | blocks ~1 s | 0 |
| T2 | `["sleep","0.5"]` | blocks ~500 ms | 0 |
| T3 | `["sleep","2m"]` | blocks ~120 s | 0 |
| T4 | `["sleep","1","2"]` | blocks ~3 s (sum) | 0 |
| T5 | `["sleep","0"]` | returns immediately | 0 |
| T6 | `["sleep","abc"]` | (stderr) invalid time interval | 1 |
| T7 | `["sleep"]` | (stderr) missing operand | 1 |

Timing is approximate (the executor can assert lower bounds with a wall
clock from the host side). Not interruptible by notes at v1.0.
