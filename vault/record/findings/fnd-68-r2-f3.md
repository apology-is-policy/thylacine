---
id: fnd-68-r2-f3
type: fnd
title: "The re-admitted wedged-server strand is not breakable by a further kill"
round: adt-68-r2
severity: P3
status: documented
surface: [sub-kernel-death]
threatens: []
seam: seam-close-flush-unbounded
regression: "none -- an honesty finding about the fix's cost"
created: 2026-08-01
---
## Prosecution

`exit_close_active` suppresses BOTH death legs for the closer. That is what
makes the close work — and it means a close-flush blocked on a wedged server
parks the dying Proc unreapably, and a further kill cannot break it out.

Pre-#68 an equivalent strand existed at reap time, where it hung the
PARENT's `wait_pid` (and therefore the shell). The fix relocates that
exposure onto the already-dying Proc, which is strictly better placed — but
it also makes it un-killable, which the round-1 write-up did not say.

## Disposition

DOCUMENTED, not fixed. The precondition is a wedged TRUSTED server — an
already system-degraded state — and the honest trade is that the alternative
(dropping the flag) reopens the data loss.

The real close is a bounded / abortable close-flush, tracked as
[[seam-close-flush-unbounded]].
