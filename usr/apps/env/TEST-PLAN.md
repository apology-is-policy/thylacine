# TEST-PLAN: env (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`env`. DEGENERATE (G15): no environment surface at v1.0.

| # | argv | output | exit |
|---|---|---|---|
| T1 | `["env"]` | (nothing -- the environment is empty) | 0 |
| T2 | `["env","FOO=bar"]` | (stderr) unsupported at v1.0 | 125 |
| T3 | `["env","ls"]` | (stderr) unsupported at v1.0 | 125 |

T1 is the honest correct output: there ARE no environment variables, so
`env` prints nothing. Setting vars / running a command needs the envp +
exec surface that does not exist (G15).
