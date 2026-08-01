---
id: fnd-68-r3-f2
type: fnd
title: "The killed-child regression's 'parked' wait was vacuous"
round: adt-68-r3
severity: P3
status: fixed
surface: [sub-kernel-death]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "sys_spawn.killed_child_delivers_pipe_eof_before_reap (comment + rename)"
created: 2026-08-01
---
## Prosecution

The regression waited for the child to be "parked" on its pipe read before
killing it. But `sys_spawn_with_fds_for_proc` installs the dup'd handles
during the PARENT's spawn call, so `handle_get(child, 0)` succeeds at
iteration 0 — before the child has run at all. The wait therefore proved
nothing about the child's state, and the kill could land pre-first-syscall.

## Disposition

FIXED as an honesty change rather than a behavioural one: the test still
discriminates on EVERY interleaving (pre-first-syscall reaches the
register-then-observe unwind; genuinely parked reaches the #811 wake; both
land in `thread_exit_self`'s close), so the correct repair was the accurate
comment plus renaming `parked` to `installed`.

A test whose NAME asserts a precondition it does not establish is a small
lie that survives longer than a wrong assertion would.
