# TEST-PLAN: true (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

`true` ignores all arguments and exits 0; produces no output.

| # | argv | stdout | exit |
|---|---|---|---|
| T1 | `["true"]` | (none) | 0 |
| T2 | `["true","anything","--help"]` | (none) | 0 (args ignored) |
