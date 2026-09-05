---
id: fnd-68-r2-f2
type: fnd
title: "thread_count is not a live-thread count, so the gate skipped joined-then-exits Procs"
round: adt-68-r2
severity: P2
status: fixed
surface: [sub-kernel-death, sub-kernel-proc]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "sys_spawn.joined_multithread_child_delivers_pipe_eof_before_reap (revert-probed against the retired gate)"
created: 2026-08-01
---
## Prosecution

`Proc.thread_count` counts UNREAPED threads: it is decremented by
`thread_unlink_from_proc`, which runs from `thread_free` at REAP. An EXITING
peer therefore still counts.

So the inherited #926 gate — `thread_count == 1` at the top of `exits()` —
does not mean "I am the only live thread". A well-formed native multi-thread
program that joins its workers and then calls `exits()` arrives with
`thread_count > 1` and `live_peers == 0`: the gate refuses, the close is
skipped entirely, and the #926 drain-before-reap deadlock survives intact on
that path.

## Disposition

FIXED: the top-of-`exits()` gate DELETED, replaced by a `live_peers`-gated
window (unlock → close → relock → recount-assert) placed before
`proc_become_zombie_locked`, the same shape `thread_exit_self` already used.

The distinction is now load-bearing in two places and is recorded as a
caveat on [[sub-kernel-proc]]: the live count is
`proc_count_live_peers_locked`, never `thread_count`.
