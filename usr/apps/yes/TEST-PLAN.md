# TEST-PLAN: yes (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`yes [STRING...]` -- repeatedly print a line forever, until the write fails.

| # | argv (+ wiring) | fd-1 stream | exit |
|---|---|---|---|
| T1 | `["yes"]`, fd 1 -> pipe, read N lines then close | `y\n` repeated | 0 (after BrokenPipe) |
| T2 | `["yes","hi","there"]`, piped | `hi there\n` repeated | 0 |
| T3 | `["yes"]`, fd 1 UNWIRED (standalone, G06) | (nothing) | 0 (first write errors -> exits immediately, no spin) |

T3 is the important safety property: with no terminal-backed fd 1 at v1.0,
yes does NOT busy-spin forever -- the first write fails and it exits.
Pair with `head` in a pipeline: `yes | head -3` -> three `y\n` lines.
