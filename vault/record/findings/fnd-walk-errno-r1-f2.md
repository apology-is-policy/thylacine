---
id: fnd-walk-errno-r1-f2
type: fnd
round: adt-walk-errno-r1
severity: P3
status: fixed
title: "A fused run's reason answered past a directory never X-checked"
surface: [sub-kernel-stalk]
threatens: [inv-i28]
fixed-by: chg-2026-11-03-walk-errno
regression: "stalk.fused_failure_keeps_the_x_gate"
created: 2026-11-03
---
## Prosecution

Stratum answers a mid-run EIO with an Rlerror for the whole op, so a run's sts[] never arrived and only the base had been checked. A caller with no search on secret read EIO for secret/x, where the per-component arm answers EACCES: a narrow, corruption-dependent existence oracle. Fixed: a reason other than a miss on a run of more than one name takes the run per component.
