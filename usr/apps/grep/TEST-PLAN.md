# TEST-PLAN: grep (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`grep [-ivnc] PATTERN [FILE...]`. PATTERN is a LITERAL substring (no regex --
there is no native regex engine; "simple" per the roadmap). Exit 0 if any
match, 1 if none, 2 on error.

Precondition: `/G` = `apple\nBanana\ncherry\napricot\n`.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["grep","ap","/G"]` | `apple\napricot\n` | 0 |
| T2 | `["grep","-i","banana","/G"]` | `Banana\n` | 0 |
| T3 | `["grep","-v","ap","/G"]` | `Banana\ncherry\n` | 0 |
| T4 | `["grep","-n","ap","/G"]` | `1:apple\n4:apricot\n` | 0 |
| T5 | `["grep","-c","ap","/G"]` | `2\n` | 0 |
| T6 | `["grep","zzz","/G"]` | (none) | 1 |
| T7 | `["grep","ap","/G","/G"]` | each line prefixed `/G:` | 0 |
| T8 | `["grep"]` | (stderr) missing pattern | 2 |
| T9 | `["grep","x"]`, stdin piped | matches from stdin | 0/1 |

Limitation: literal-substring only -- regex (`.`, `*`, `[...]`, anchors)
is NOT supported (no regex crate in the native no_std environment).
