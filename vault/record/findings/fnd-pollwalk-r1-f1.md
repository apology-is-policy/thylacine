---
id: fnd-pollwalk-r1-f1
type: fnd
round: adt-pollwalk-r1
severity: P1
status: fixed
title: "The bookmark waits spun while their preemptible drainer sat runnable on the same CPU"
surface: [sub-kernel-poll]
threatens: [inv-i8]
fixed-by: chg-2026-10-28-poll-hook-walks
regression: "poll.await_yields_to_a_drainer_on_its_cpu (both threads pinned to one CPU, the drainer parked between batches, the awaiter at in_syscall = 1); sabotage S18 hangs it"
created: 2026-10-28
---
## Prosecution

The chunk made every owed walk preemptible, so its drainer -- the console manager
posting Ctrl-C's note, the dev9p poll kthread draining a Loom's CQ walk -- can be
preempted between batches with its bookmark listed. `proc_free` (wait_pid) and
`loom_free` (close) waited with the ARM `yield` hint, in a syscall body that
`preempt_check_irq` never preempts. On -smp 1 nothing rescues the drainer.

## Fix

`poll_await_pause`: `sched_yield_hint()` while anything else can run on this
CPU, the hint only when nothing can. The third shape of the lesson the
loom_free join taught: a spinner must yield the CPU, not the core.
