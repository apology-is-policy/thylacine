---
id: fnd-net8d-r1-f3
type: fnd
title: "resident_lo_selftest FAILs non-fatally — netd serves /net over a failed loopback proof"
round: adt-net8d-r1
severity: P3
status: deferred
surface: [sub-netd-nic]
threatens: []
seam: seam-242-selftest-nonfatal
created: 2026-07-31
---
## Prosecution

`serve()` only logs a FAIL line on a non-PASS selftest and proceeds to
post `/srv/net`. Ship risk is ~zero (the net-echo boot gate exits
non-zero on a broken loopback, so a regression cannot ship silently) —
the finding is that the fail-closed posture belongs at netd's OWN
layer, like `post_srv_net`'s.

## Disposition

Deferred → [[seam-242-selftest-nonfatal]] (task #242): return Err on a
deterministic-selftest FAIL (flake-safe — the battery is host-decoupled
by design).
