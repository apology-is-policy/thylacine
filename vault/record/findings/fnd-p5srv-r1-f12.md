---
id: fnd-p5srv-r1-f12
type: fnd
title: "client_fid uninitialized at create — soundness rested on the handshake_done gate alone"
round: adt-p5srv-r1
severity: P3
status: documented
surface: [sub-kernel-srvconn]
threatens: []
created: 2026-07-31
---
## Prosecution

The era's embedded per-conn 9P client left `client_fid` implicitly
zeroed; every r/w fail-closed through the `client_handshake_done` gate,
but the pattern was fragile against a future caller bypassing the gate.

## Disposition

Documented at the time; RETIRED WHOLESALE at
[[chg-2026-06-03-stalk3b-open-connect]] (D) — the embedded client,
`client_fid`, and `client_handshake_done` no longer exist; the SrvConn
is pure transport + identity and 9P state lives in the shared kernel
client. The finding survives only as do-not-re-report history.
