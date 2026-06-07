# TEST-PLAN: false (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`false` ignores all arguments and exits 1; produces no output.

| # | argv | stdout | exit |
|---|---|---|---|
| T1 | `["false"]` | (none) | 1 |
| T2 | `["false","anything"]` | (none) | 1 (args ignored) |
