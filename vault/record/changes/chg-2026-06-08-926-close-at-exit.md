---
id: chg-2026-06-08-926-close-at-exit
type: chg
title: "#926: a single-thread Proc closes its fds at exit, not at reap"
date: 2026-06-08
arc: arc-go-build
commits: ["e9e0aa92"]
touched: [sub-kernel-death, sub-kernel-proc]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- the dossiers are established in this same sweep commit"
---
## What

`proc_close_handles_at_exit` — a single-thread Proc closes and frees its
handle table at the top of `exits()`, gated `thread_count == 1`, instead of
leaving it to `proc_free` at reap.

## Why

A shell draining `$(cmd)`'s stdout to EOF hung forever. The child's pipe
write end stayed open in the ZOMBIE until the parent reaped it, so
`write_eof` was never delivered — and the parent could not reap, because it
was blocked draining. EOF needed the reap; the reap needed the wait; the
wait waited on EOF.

The fix's whole difficulty is WHERE. The close must run while the thread is
still RUNNING (a 9P clunk's Tclunk/Rclunk wait may SLEEP, and sleeping while
EXITING trips `sched()`'s assert) and while the Proc is still ALIVE (so the
reaper cannot free the closer mid-close), with exactly one toucher of the
table. Those three properties define the window, and they are the same three
#68 later re-derived for the multi-thread case.

The ordering inversion this creates — handle close at exit, `vma_drain` at
reap — is safe only because of the #847 per-Burrow dual refcount: dropping
`handle_count` while a VMA still maps does not free.

## Verification

[[adt-926-r1]] — one Opus prosecutor + a concurrent self-audit, CLEAN
0/0/0/3 (all documentation). Its F2 recorded the asymmetry this chunk left
open — a KILLED single-thread Proc still deferred to reap — as a v1.x
EXITING-protocol item, which [[chg-2026-07-14-68-last-thread-out-close]]
closed.
