---
id: fnd-net6b-r1-f3
type: fnd
title: "The 20 ms idle reader-pump wakes at 50 Hz while a probe is parked"
round: adt-net6b-r1
severity: P3
status: deferred
surface: [sub-kernel-ninep-dev9p-poll]
threatens: []
seam: seam-221-idle-pump-wake
created: 2026-07-31
---
## Prosecution

A deferred poll parked on a never-ready socket costs the kthread a 50 Hz
wake indefinitely (the frame-boundary deadline re-poll).

## Disposition

Deferred: the deadline is LOAD-BEARING (it is what lets the kthread GC
stranded ops and detect widens); the fix is a transport wake-on-write.
Tracked as task #221; the netd half was trimmed 2026-06-21 (c1e49fb1).
