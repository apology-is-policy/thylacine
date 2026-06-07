# TEST-PLAN: which (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`which NAME...`. DEGENERATE (G15/G07): no PATH and no cwd to search. A NAME
with '/' is probed as an explicit path; a bare name cannot be resolved.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["which","/sbin/login"]` (exists) | `/sbin/login\n` | 0 |
| T2 | `["which","/nope"]` | (none) | 1 |
| T3 | `["which","ls"]` (bare name) | (none -- no PATH) | 1 |
| T4 | `["which"]` | (none) | 1 |

T3 demonstrates the gap: without a PATH variable there is nothing to
search, so a bare command name is always "not found".
