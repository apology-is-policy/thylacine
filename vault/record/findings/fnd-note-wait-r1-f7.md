---
id: fnd-note-wait-r1-f7
type: fnd
round: adt-note-wait-r1
severity: P3
status: fixed
title: "NOHAND re-issues forever on an sp the tail refuses"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
fixed-by: chg-2026-11-03-note-wait
regression: "notewait.suspend_bad_sp_keeps_eintr; sabotage K25"
created: 2026-11-03
---
## Prosecution

With sp out of bounds the tail returns before touching the queue, the settle sees nothing caught and rewinds, and the svc finds the same note. The thread burns a CPU without executing an EL0 instruction that would fault.

## Disposition

The settle refuses the rewind when the tail refuses the sp, so the call answers EINTR as it did before.
