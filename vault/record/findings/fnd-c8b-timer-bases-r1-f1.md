---
id: fnd-c8b-timer-bases-r1-f1
type: fnd
round: adt-c8b-timer-bases-r1
severity: P3
status: fixed
title: "thread_release has no gate on a thread still on a timer base"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-03-c8b-timer-bases
regression: "none: it needs a caller that frees a sleeper, and the extinction is the answer to one"
created: 2026-11-03
---
## Prosecution

Freeing a SLEEPING thread in a deadlined tsleep leaves an entry the next tick walks into freed memory. Inherited from g_timerwait, and test-only reachable today. Fixed: thread_release extincts on a set mark.
