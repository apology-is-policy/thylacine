---
id: fnd-p5srv-r1-f7
type: fnd
title: "Claimed name_va buffer overrun in the post handler"
round: adt-p5srv-r1
severity: P3
status: withdrawn
surface: [sub-kernel-devsrv]
threatens: []
created: 2026-07-31
---
## Prosecution

(As filed) The post handler's name copy could overrun its kernel-stack
scratch for an attacker-chosen `name_len_raw`.

## Disposition

WITHDRAWN by the prosecutor's own self-check:
`if (name_len_raw == 0 || name_len_raw > SRV_NAME_MAX) return -1`
bounds the copy before any byte moves — the chain did not survive
re-reading the guard. Recorded so the bound is on the do-not-re-report
set.
