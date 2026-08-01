---
id: chg-2026-07-17-pty1-sessions-groups
type: chg
title: "PTY-1a/1e/1f: sessions, process groups, the wait extension, and the second stop owner"
date: 2026-07-17
arc: arc-pty
commits: ["a418dba0", "f2ee7f66", "bce3fe33"]
touched: [sub-kernel-proc, sub-kernel-death]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- the dossiers are established in this same sweep commit"
---
## What

`sid`/`pgid` on `struct Proc` (rfork-INHERITED, unlike every debug/report
field around them) plus the four POSIX cores; `wait_pid_for`'s pgrp
selectors, `WAIT_UNTRACED`/`WAIT_CONTINUED`, the packed Linux-layout status,
and the report latches; and `job_stop_req` — a SECOND, independent stop
owner sharing the debugger's park.

## Why

Job control needs three things the kernel did not have: a session/group
identity per Proc, a way for a parent to observe a stop without reaping, and
a way to suspend a group.

The **two-owner park** is the design's sharp edge. A thread parks iff
`debug_stop_req | job_stop_req`, and each resume clears ONLY its own flag —
so a `tty:cont` can never run a debugger-stopped thread and a detach can
never run a Ctrl-Z'd one (`StopCompatI39`). Death overrides both.

**Report-is-not-reap** (R2-F6): a stop/continue report returns the child's
pid and a packed status and consumes the latch, but runs NONE of the
unlink/thread_free/proc_free teardown — the child stays linked and ALIVE,
reapable later. Only an exit reaps.

The **catchability gate** (R2-F3) decides per member whether an uncaught
`tty:susp` means a note or a stop: a handler, the notes fd, or an all-masked
thread set means the note delivers on the target's own terms and NO stop
happens (tmux and bash catch SIGTSTP to save terminal state). Uncaught and
orphaned means discarded entirely — the POSIX orphan rule's suppression
half, since nobody could resume it.

The whole privileged surface stays kernel-side: `ptyfs` can name a pts, never
a pgrp.

## Verification

Retro record from `git log` + PTY-DESIGN.md §4 + the code. Regressions
`proc.job_stop_owner_algebra`, `proc.job_stop_orphan_rule`,
`proc.job_stop_park_report_cont_live`, `proc.wait_pid_for_pgrp_selectors`,
`proc.wait_pid_for_report_not_reap`; pinned by `pty_stop.tla`
(`StopCompatI39` + `DeathWinsOverJobStop` + the `BUGGY_DOUBLE_STOP`
counterexample), which lands with the PTY area's own sweep.
