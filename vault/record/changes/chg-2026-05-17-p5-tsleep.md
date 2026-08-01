---
id: chg-2026-05-17-p5-tsleep
type: chg
title: "P5-tsleep: the deadline-bounded Rendez sleep, and the one-at-a-time wake"
date: 2026-05-17
arc: arc-corvus-srv
commits: ["33bfb059", "920434d9"]
touched: [sub-kernel-rendez]
established: []
closed: []
opened: [seam-timerwait-sharding]
mirrors-checked: []
depth: skeletal
---
## What

`tsleep(r, cond, arg, deadline_ns)` — `sleep` with a deadline, and the
global timer-wait list that delivers it off the 1 kHz tick. The kernel
primitive behind every bounded wait: a `/srv` client blocked on a
possibly-hung 9P server, and later the poll and futex timeouts.

The second commit is the F6 fix from its own audit: `timerwait_tick`
wakes expired sleepers **one at a time**, releasing the global lock
between wakes, so a burst of simultaneous timeouts cannot stall every
other CPU's tick behind one long hold. Each individual wake stays atomic
— both locks held continuously across it — which is what the model needs;
the batching is a scheduling concern the model does not see.

## Why it is delicate

`sleep` has two wake sources serialized by one lock. `tsleep` adds a
**third**, from a different lock and from IRQ context, so three actors
race for one waiter. The rules that fall out:

- **`cond` has precedence.** A wait satisfied exactly as the deadline
  lapses reports AWOKEN, because the loop checks the condition first.
  Checking the deadline first looks equivalent and silently converts a
  satisfied wait into a spurious timeout.
- **`wakeup` takes the global lock as its OUTER lock** even for a plain
  `sleep` waiter that is never on the list — it cannot know until it
  holds `r->lock`, and by then the order would be inverted.
- **`timerwait_tick` pre-filters `on_cpu`**, so the wake's spin never
  runs inside the timer IRQ handler.

[[spec-tsleep]] is the model; its four buggy cfgs are the four ways to
get this wrong.
