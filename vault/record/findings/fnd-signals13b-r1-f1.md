---
id: fnd-signals13b-r1-f1
type: fnd
round: adt-signals13b-r1
severity: P1
status: fixed
title: "The seam-check list was not extended for the five note syscall numbers (the threads-round F5, verbatim)"
surface: [sub-pouch-seam, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-05-24-p6-signals-b
regression: "the five note entries + five sentinel representatives in the seam check"
created: 2026-08-01
---
## Prosecution

Identical in shape to [[fnd-threads9b-r1-f5]] one round earlier: 0007
adds `__NR_note_open 44` … `__NR_note_mask 48`, and the build's static
verification list was not extended. A re-vendor losing any of the five
would pass the check, and pouch's signal layer would map to `0xFFFF` ->
`ENOSYS` — programs failing mysteriously with no build-time signal.

That it recurred one round later, in the same codebase, with the earlier
finding already in the closed list, is the finding's real content: the
obligation was recorded against the ROUND rather than against the
mechanism.

## Fix

The five numbers added, PLUS the sentinel-mapped Linux signal names
(`rt_sigaction`, `rt_sigprocmask`, `tkill`, `kill`, `rt_sigreturn`
= `0xFFFF`) so drift is detected in BOTH directions — a number appearing
where a sentinel belongs is equally wrong. The obligation now lives on
[[sub-pouch-seam]].
