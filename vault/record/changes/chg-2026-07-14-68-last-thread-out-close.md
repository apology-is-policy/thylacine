---
id: chg-2026-07-14-68-last-thread-out-close
type: chg
title: "#68: the LAST live Thread out closes the fds — multi-thread and killed Procs deliver EOF at exit"
date: 2026-07-14
arc: arc-go-build
commits: ["b3bd5f5f"]
touched: [sub-kernel-death, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: [seam-exiting-tails-never-sleep, seam-close-flush-unbounded]
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- sub-kernel-death is established in this same sweep commit; sub-kernel-ninep-dev9p is the write-behind flush this chunk repaired, swept in batch 3"
---
## What

The #926 close window generalizes to EVERY exit path. The `thread_count == 1`
gate is retired; both `exits()` and `thread_exit_self()` open a
`live_peers == 0` window (unlock → close → relock → recount-assert) BEFORE
the ZOMBIE flip. The close runs under a new per-Thread `exit_close_active`,
which makes `thread_die_pending()` read false for the closer.

## Why

The Go toolchain promoted the documented asymmetry to a v1.0 defect: every
Go binary exits MULTI-thread via `SYS_EXIT_GROUP`, so its pipe write ends
delivered EOF only at REAP and the #926 deadlock survived intact on that
path — the nora gofmt-on-save hang and the ut `$(go ...)` substitution
wedge, session-unrecoverable.

The fingerprint that separated it from every other hang: native substitution
completed while Go substitution wedged, and ut PIPELINES passed — because
the WNOHANG sweep reaps the Go element while a SIBLING waits for EOF, so
reaper and EOF-waiter were different threads.

Two gates turned out to be wrong for the same reason. `thread_count` counts
UNREAPED EXITING peers and decrements only at reap, so it is not a live
count: a joined-then-exits native multi-thread program arrives at `exits()`
with `thread_count > 1` and `live_peers == 0`, and the old gate skipped its
close entirely (R2-F2).

## Verification

Three Fable rounds, CONVERGED CLEAN: [[adt-68-r1]] (0/1/1/2) →
[[adt-68-r2]] (0/1/1/2, on the R1 fixes) → [[adt-68-r3]] (0/0/0/2). Both
P1s were the same class — a premise about when the death machinery is armed
that turned out false. Regressions
`sys_spawn.killed_child_delivers_pipe_eof_before_reap` and
`sys_spawn.joined_multithread_child_delivers_pipe_eof_before_reap`, both
revert-probed; the go6 E2E.
