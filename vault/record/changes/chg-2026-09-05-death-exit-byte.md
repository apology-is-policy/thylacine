---
id: chg-2026-09-05-death-exit-byte
type: chg
title: "sub-kernel-death brought current: #91 -- the ZOMBIE chokepoint captures the real exit byte, not the 0/1 collapse"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-kernel-death
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The last proc.c-cluster sibling with EARNED churn (jobctl remains, likely
borrowed -- see below). The dossier's own Provenance had recorded the 2026-08-16
interval as borrowed; #91 (`f557beb2`, 2026-08-26) is the next earned one, and it
is squarely death's -- the exit-status source. Verified against the code.

## The #91 exit byte (the C1 self-hosting floor's exit half)

Before #91 a process's integer exit status collapsed to 0/1 at two kernel points
(sys_exits_handler, sys_exit_group_handler), so a `t_exits(42)` or a phenotype
`exit_group(42)` reached the parent's wait as 0 or 1. #91 fixed it:

- `Proc.exit_status` (proc.h 177) is a separate int; `proc_become_zombie_locked`
  stores it VERBATIM (proc.c 2675), and `notes_post_child_exit` (2692) + the wait
  reap (`WAIT_STATUS_EXITED`, 4608) carry the real byte.
- `exits(msg)` (2923) now maps `"ok"`->0 / else->1 and calls `exits_code(code,
  msg)` (2928); the ~dozen in-kernel `exits("...")` callers keep the 0/1 mapping,
  while `SYS_EXITS`/`SYS_EXIT_GROUP` call `exits_code` directly with the real byte.

The dossier's Data structures said `group_exit_msg` was "both the die flag and
the last-out status source (`"ok"`->0, else 1)" -- the OLD collapse. Split there:
`group_exit_msg` is the die flag + msg; `exit_status` is the numeric status. Added
a Mechanism paragraph ("The exit byte is the real one now") at the ZOMBIE
chokepoint. The wait-PACK half (`WAIT_STATUS_EXITED`) is [[sub-kernel-proc]]'s
`wait_pid_for` (its "packed status" prose stays accurate, just generic -- a
future proc touch could add the encoding detail; not owed here).

## What was NOT stale

The vfork park, the death-wake cascade, the exec-time disposition reset (a
use-after-free once, now reset-in-place) are all current -- the dossier already
carries them. `updated:` -> 2026-09-05. guarded-by unchanged [inv-i24, inv-i9,
inv-i44].

## The last cluster sibling: jobctl

sub-kernel-jobctl is flagged stale by the same shared proc.c churn but looks
BORROWED: setsid/setpgid/notes_post_pgrp/job_stop are UNCHANGED since 2026-08-16.
A substring scan hit noisy sid/session/pgrp (false-positive-prone), so confirming
it needs a WORD-BOUNDED token diff in context before a borrowed-re-verify (the
caps stale-by-cotenancy pattern) -- NOT assumed here. Left for that check.
