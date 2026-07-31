---
id: fnd-weft7-r1-f4
type: fnd
title: "The netd raw-pointer ring sites' single-threadedness preconditions were undocumented"
round: adt-weft7-r1
severity: P3
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-21-weft7-close
created: 2026-07-31
---
## Prosecution

The raw-slice sites (`weft_recv_into_ring`'s mut-slice + ready_signal
write; `h_weftio`'s TX slice) are sound in two layers with different
grounds: the VALUE safety (a vanished/shrunk ring yields Eof/E_INVAL,
never OOB) holds unconditionally, but the mapping LIVENESS of
`ring_va` against a concurrent `slot_unref → t_burrow_detach` rests
entirely on netd being SINGLE-THREADED — and that precondition was
nowhere stated at the sites, so a future concurrency lift (the A-5b
per-user-netd config) could silently void it.

## Disposition

Fixed (doc hardening): explicit "INVARIANT (Weft-7 F4)" notes at the
three sites — a lift MUST add a per-slot guard keeping the ring mapped
across the raw access — plus the WeftFlow `!Send`/`!Sync` rationale.
The prosecutor independently confirmed the property itself HOLDS; the
notes are what make it survivable. Carried forward as the
[[sub-netd-server]] Concurrency obligations.
