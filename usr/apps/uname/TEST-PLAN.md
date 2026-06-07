# TEST-PLAN: uname (A3)

Status: authored, NOT executed. Verified: cargo build + clippy clean.

`uname [-asnrvm]`. STATIC fields (G16: no uname/sysinfo syscall). Default -s.

| # | argv | fd-1 stdout | exit |
|---|---|---|---|
| T1 | `["uname"]` | `Thylacine\n` | 0 |
| T2 | `["uname","-m"]` | `aarch64\n` | 0 |
| T3 | `["uname","-a"]` | `Thylacine (none) 1.0-dev #1-thylacine aarch64\n` | 0 |
| T4 | `["uname","-sm"]` | `Thylacine aarch64\n` | 0 |
| T5 | `["uname","-Z"]` | (stderr) invalid option | 1 |

The release ("1.0-dev") and version are HARDCODED, not read from the
kernel (G16); nodename is "(none)" (no hostname surface).
