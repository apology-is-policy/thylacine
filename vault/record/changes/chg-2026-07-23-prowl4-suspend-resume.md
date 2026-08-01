---
id: chg-2026-07-23-prowl4-suspend-resume
type: chg
title: "prowl-4: job-control suspend/resume by pid through /proc/<pid>/ctl"
date: 2026-07-23
arc: arc-pty
commits: ["ee8baee5"]
touched: [sub-kernel-death]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "retro backfill -- sub-kernel-death is established in this same sweep commit"
---
## What

`suspend` / `resume` verbs on `/proc/<pid>/ctl`, reusing the PTY-1f
per-member helpers (`proc_job_stop_proc` / `proc_job_cont_proc`) behind the
SAME I-26 two-axis gate as `kill`.

## Why

A monitor pausing an arbitrary process by pid. No new authority and no new
invariant: stopping is strictly WEAKER than the killing that gate already
authorizes, so reusing it is the conservative choice rather than a shortcut.

Deliberately UNCONDITIONAL — no `tty:susp` note, no catchability gate. This
is the Plan 9 `stop`, uncatchable exactly as the `/proc` `kill` is. The
report latches still fire, so the target's PARENT sees the
`WAIT_UNTRACED`/`WAIT_CONTINUED` edge, which is correct: whoever stops a
process, its parent's wait reports the stop, and a non-opt-in parent simply
ignores the latch.

## Verification

Retro record from `git log`. A `/proc`-stopped process that is later
orphaned is cleaned by the existing POSIX orphan rule at its parent's zombie
flip — no new hazard.
