---
id: dec-2026-10-13-exit-wins-final-close
type: dec
title: "A kill that lands during a Proc's final close forces the close but leaves the exit's own status, on both last-out paths"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-kernel-death, sub-kernel-proc]
created: 2026-10-13
---
## Fork

A Proc's last thread closes its handles in the final close (ARCHITECTURE
7.9.1). A `proc_group_kill` landing in that window forces the close (part B).
Which status does the Proc reap with? `exits_term` checks the group claim once,
before the close, and then records its own exit; `thread_exit_self`'s last-out
(a last thread leaving by `SYS_THREAD_EXIT`) reads the claim after the close,
so there the kill's `SIGKILL` wins. (gcpark-b audit F5; pre-existing for the
message and code.)

## Research

- Linux: a task that has entered `do_exit` is `PF_EXITING`; a fatal signal
  that arrives then does not rewrite the exit code it reports.
  `do_group_exit` lets a group exit already in progress keep its code.
- The `exits_term` path already follows that shape: the claim check runs under
  the same hold as the `PROC_FLAG_EXIT_CLOSING` mark, so a kill lands before it
  (the kill's claim wins) or after it (it forces the close, the exit stands).

## Options

- **A. The exit wins**: both last-out paths read the claim once, when the
  close is marked, and a later kill only forces the close.
- **B. The kill wins**: both paths read the claim after the close.

## The call

A (operator vote, 2026-10-13). ARCHITECTURE I-24 and 7.9.1 part B and
ERRORS.md say so; a kernel test kills a child inside its forced close on each
path.

## Rationale

The exit had already decided the status before the kill arrived; the kill's
only remaining job is to end the close.
