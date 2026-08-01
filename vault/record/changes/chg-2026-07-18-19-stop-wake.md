---
id: chg-2026-07-18-19-stop-wake
type: chg
title: "#19: the stop cascade gets its own non-completing torpor walk"
date: 2026-07-18
arc: arc-pty
commits: ["658a46f6"]
touched: [sub-kernel-torpor]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The PTY-4e close's root fix. The job-control STOP cascade had reused
`torpor_wake_all_for_proc` — the DEATH walk, which sets `awoken` and
COMPLETES every wait. Correct for death (the Proc dies at its
EL0-return tail; the fabricated `TORPOR_OK` is never observed);
wrong for a SURVIVING stop: every torpor-timed wait completed
spuriously on fg/`SYS_TTY_CONT` resume. The visible symptom: a
`time::sleep`-based `/bin/sleep` under Ctrl-Z + fg "finished"
instead of continuing.

The fix is `torpor_stop_wake_all_for_proc`: identical walk and lock
discipline, but `awoken` stays CLEAR — the woken thread's cond
re-check fails, its tsleep re-loop hits the stop detour, and it
parks with the wait PRESERVED, re-registering with its original
deadline on resume (parks-and-reparks; the Linux
SIGSTOP-over-futex_wait restart shape). A REAL wake landing during
the stop-park still delivers: the waiter stays bucket-linked across
the park, the poster sets `awoken`, the resumed re-loop returns
AWOKEN immediately.

The one-bit difference between the two walks — whether `awoken` is
set — is the whole semantic distance between "this Proc is dying"
and "this Proc will be back".
