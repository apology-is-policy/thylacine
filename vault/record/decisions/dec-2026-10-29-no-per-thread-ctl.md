---
id: dec-2026-10-29-no-per-thread-ctl
type: dec
title: "No per-thread ctl: a thread is stepped by `step <tid>` on the Proc ctl"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc]
created: 2026-10-29
---
## Fork

The Fable thread-model review's per-thread directory listed `{.., ctl}` beside
the register files. The debug slot is owned by the open Proc-level `ctl` Spoor
(`debug_owner` is that pointer, DEBUG-FS-DESIGN 7.2), so a second control file
could not show that it belongs to the debugger holding the slot. Options: no
per-thread ctl, with a tid operand on the Proc ctl's `step` (Linux
`PTRACE_SINGLESTEP(tid)` shape); a per-thread ctl that re-proves ownership by
some token; a per-thread ctl for reads only.

## Decision

Operator vote V1 (2026-10-29 ~08:37Z): no per-thread ctl. `step <tid>` on the
Proc ctl steps the named thread; `step` alone keeps the focus. Stopping and
starting apply to the whole Proc. A thread object writes only its register
files, so no control kind is reachable through one ([[sub-kernel-devproc]]).
