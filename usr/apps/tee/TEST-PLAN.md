# TEST-PLAN: tee (A1)

Status: authored, NOT executed. Verified: `cargo build --release` + clippy
clean. First user of the aux-rt::fs create shim (DOC-GAP G09).

`tee [-a] [FILE...]` -- copy stdin to stdout AND to each FILE (created /
truncated, mode 0644). Needs fd 0 wired (a pipe) to be useful; fd 1
visibility per G06. Output files need a WRITABLE filesystem (dev9p; the
boot ramfs is read-only, so file creation there returns an error).

Preconditions: a writable directory (e.g. a dev9p mount); fd 0 piped.

| # | argv (+ wiring) | effect | exit |
|---|---|---|---|
| T1 | `["tee","/w/out"]`, stdin `hello\n` | stdout `hello\n`; `/w/out` == `hello\n` | 0 |
| T2 | `["tee","/w/a","/w/b"]`, stdin `x` | stdout `x`; `/w/a`==`/w/b`==`x` | 0 |
| T3 | `["tee"]`, stdin `data` | stdout `data` (no files; like cat) | 0 |
| T4 | `["tee","-a","/w/out"]`, stdin `y` | stderr warning (append degrades to truncate, G09); `/w/out`==`y` | 0 |
| T5 | `["tee","/ro/out"]` on a READ-ONLY fs | stderr create error; stdout still gets stdin | 1 |
| T6 | `["tee","/w/x","--","-weird"]` | creates `/w/x` and `/-weird` (-- ends options) | 0 |

Notes:
- Append is intentionally NOT silently wrong: -a warns and truncates,
  because no append surface exists at v1.0 (G09).
- A create failure for one file does not abort the others or stdout; tee
  exits 1 but still streams to the successful sinks.
