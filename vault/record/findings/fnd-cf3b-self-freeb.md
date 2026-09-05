---
id: fnd-cf3b-self-freeb
type: fnd
title: "The all-or-nothing send's free-space bound still read the compile-time ring cap — the first bulk frame never fit"
round: adt-cf3b-r1
severity: P1
status: fixed
surface: [sub-kernel-srvconn]
threatens: []
fixed-by: chg-2026-07-08-cf3b-bulk-ring
regression: srvconn.bulk_ring_class
created: 2026-07-31
---
## Prosecution

Pre-audit, in-chunk (self-found before commit): after the per-conn heap
rings landed, `srvconn_client_send_frame`'s free-space computation still
read the compile-time `SRVCONN_RING_CAP` (65536) instead of `ch->cap` —
so on a bulk conn the FIRST 128 KiB Twrite frame "never fit" an empty
512 KiB-capable ring. `client_send_flow` EAGAIN-spun, the self-pump
blocked on a reply that was never requested: a whole-boot wedge at
fsbench's first bulk write.

## Disposition

Fixed before landing — found by GROUND TRUTH (the boot hang plus a
failed edit assertion exposing the stale line), not the theory loop; the
"no stale consumers" grep had excluded the very file
(`grep -v kernel/srvconn.c`) — a sloppy-exclusion grep is not a
completeness proof. Pinned by the in-test big-frame regression
(`srvconn.bulk_ring_class` sends a 96 KiB frame whole; fails pre-fix by
construction). The cap-must-be-`ch->cap` rule is on
[[sub-kernel-srvconn]]'s Prosecution list.
