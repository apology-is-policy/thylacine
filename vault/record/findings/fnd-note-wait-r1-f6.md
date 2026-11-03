---
id: fnd-note-wait-r1-f6
type: fnd
round: adt-note-wait-r1
severity: P3
status: deferred
title: "A SIGPIPE default set inside a handler loses its mask half"
surface: [sub-kernel-notes, sub-pouch-signal]
threatens: []
regression: "none yet"
created: 2026-11-03
---
## Prosecution

Pouch's SIGPIPE default rides on the mask. signal(SIGPIPE, SIG_DFL) in a handler sets the bit in the handler's mask, SYS_NOTED restores the old mask with the bit clear, and the next write to a closed pipe terminates where POUCH-DESIGN 6.4 promises EPIPE. The restore is POSIX-correct; the emulation is what breaks.

## Disposition

Stated in sub-pouch-signal's caveats and tracked in OPEN-BUGS. The fix is a default row that ignores, which needs no mask and also closes seam-pouch-sigmask-per-thread. That changes a RESOLVED item in POUCH-DESIGN 6.4, so it goes to the operator.
