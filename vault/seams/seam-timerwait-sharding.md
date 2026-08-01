---
id: seam-timerwait-sharding
type: seam
title: "One global lock and list for every deadlined sleeper"
status: open
surface: [sub-kernel-rendez]
opened-by: chg-2026-05-17-p5-tsleep
tracker: "documented future optimization"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Per-CPU sharding of `g_timerwait`. Today one global lock protects one
global list of every thread inside a deadlined `tsleep`, and
`timerwait_tick` runs on **every CPU's** tick.

## Why it is this way

Deliberate, and defended on three grounds: a deadlined wait is the cold
path (a hung-server backstop, poll and futex timeouts); the scan is
O(timed sleepers), which is small; and the global lock is what
[[spec-tsleep]] actually verifies — sharding would need the model
extended before the code.

## Cost of leaving it

Two shapes, both bounded:

- **Contention.** Every CPU's tick takes the lock, and `wakeup` takes it
  as its outer lock even for a plain `sleep` waiter that is never on the
  list (it cannot know until it holds `r->lock`, by which point taking
  the global lock would invert the order). Two mitigations are already
  in: `wakeup` releases it the moment its unlink is done, and
  `timerwait_tick` wakes **one at a time**, re-acquiring per iteration,
  so a burst of simultaneous timeouts cannot stall other CPUs behind one
  long hold (the P5-tsleep F6 fix).
- **The rescan.** `timerwait_tick` rescans from head each iteration, so
  it is O(n²) in the per-tick herd size. Sharding makes it O(n).

Neither is a correctness issue, and both are invisible at v1.0 loads. The
one thing to preserve if it is ever sharded: the lock is also read by
`timerwait_earliest_deadline` from the *tickless idle* path, as a leaf
acquisition, and that reader deliberately does **not** filter `on_cpu`
(it reads deadlines and wakes nothing). A sharded design still owes a
correct global minimum for the one-shot arm.
