---
id: arc-pty
type: arc
title: "PTY + job control: sessions, groups, and the second stop owner"
status: active
design: ["docs/PTY-DESIGN.md"]
chunks:
  - chg-2026-07-17-pty1-sessions-groups
  - chg-2026-07-23-prowl4-suspend-resume
follow-ons: []
created: 2026-08-01
---
## Goal

Give Thylacine POSIX job control: sessions and process groups on the Proc,
a pseudoterminal server, and the ability to suspend and resume a foreground
job — without letting the userspace terminal server name a process group
directly.

The kernel half is a deliberately small, security-shaped seam: `ptyfs` owns
the bytes and the line discipline, but its ONLY signal authority is the
pts-scoped `SYS_TTY_SIGNAL`. It can never name a pgrp; the kernel resolves
pts → session → foreground group itself. Everything privileged stays inside
[[lock-proc-table]].

## Planned chunks

- **PTY-1a/1e/1f** — the kernel seam: `sid`/`pgid` on the Proc (rfork-
  inherited, POSIX fork semantics), the `SYS_SETSID`/`SETPGID`/`GETPGID`/
  `GETSID` cores, the wait extension (`WAIT_UNTRACED`/`WAIT_CONTINUED`, the
  pgrp selectors, and report-is-not-reap), and `job_stop_req` — the SECOND
  stop owner, sharing the debugger's park with per-owner clears.
- **prowl-4** — job-control suspend/resume by pid through
  `/proc/<pid>/ctl`, under the same I-26 two-axis gate as `kill`;
  unconditional and uncatchable, the Plan 9 `stop`.

## Close summary

Recorded from the 2026-08-01 sweep, which swept the KERNEL half only
(`kernel/proc.c`'s session/group/job-stop machinery). The userspace `ptyfs`
server, the pouch pty boundary-line, and the PTY-2/3/4 chunks land with
their own area sweeps — the chunk list here will grow.

The design's sharpest property, and the one to preserve: **two independent
stop owners, one park.** A thread parks iff `debug_stop_req | job_stop_req`,
and each resume clears only its own flag, so a `tty:cont` can never run a
debugger-stopped thread and a detach can never run a Ctrl-Z'd one. Death
overrides both.
