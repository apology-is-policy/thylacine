# TEST-PLAN: pwd (A1)

Status: authored, NOT executed. Verified: `cargo build --release` clean.

DEGENERATE (DOC-GAP G07): Thylacine v1.0 has no per-Proc cwd. pwd prints
"/" (the territory root, the only working-directory anchor).

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["pwd"]` | `/\n` | 0 |

When a real per-Proc cwd lands, this test gains cases (after a `cd` the
output tracks the new directory). At v1.0 the output is invariant.
