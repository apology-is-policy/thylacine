---
id: dec-2026-10-07-exit-close
type: dec
title: "The at-exit close: the clunk never waits; a kill during the final close hands the rest to the closer, as Plan 9's forceclosefgrp does"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-death, sub-kernel-ninep-client, sub-kernel-loom, seam-close-flush-unbounded]
created: 2026-10-07
---
## Fork

A dying Proc closes its handles under `exit_close_active`, which suppresses
death so a write-behind flush gets its reply (#68 F1). Any process can serve
a 9P mount (`SYS_ATTACH_9P` gates only on the fds), so a server that stops
answering holds the dying Proc at five wait sites -- the elected reader's
recv, the reply sleep, the self-pump, the progress park and the Loom SQPOLL
reap's join -- and a second kill cannot break in
([[seam-close-flush-unbounded]]).

## Research

- **Plan 9 (4e, 9front).** No timeout: `mountio` re-waits through a Tflush
  chain. The escape is kill escalation: `closefgrp` sets `up->closingfgrp`,
  and a kill landing in `sleep()` calls `forceclosefgrp`, which hands every
  still-open chan to the `ccloseq` close-queue kprocs -- "the blocked cclose
  that we've interrupted will finish by itself".
- **Linux 9p.** `io_wait_event_killable` plus a Tflush; a clunk retries once,
  then destroys the fid (accepting a fid leak on the server).
- **FUSE.** A forced request is waited out; the escape is aborting the
  connection or the server closing `/dev/fuse`.
- **Zircon and Mach.** Death never waits on a server: handles and ports are
  destroyed and the server is notified.

## Options

1. **A now, then B with C.** A: the clunk side never waits at exit (the clunk
   goes to the closer; nothing is lost). B with C: a kill during the final
   close hands the rest of it, the write-behind run included, to the closer
   kthread; the Proc is reaped at once and the data is still written when the
   server answers.
2. **A only.** The write-behind flush still holds a killed Proc until the
   server answers or dies.
3. **A, then B with discard.** A second kill abandons the flush and discards
   the staged bytes loudly.
4. **A per-close deadline.** Drops an honest slow server's writes; no
   heritage or peer system does it.

## The call

Option 1 (operator, 2026-10-07, AskUserQuestion). The chunk follows the
tag-pool chunk ([[dec-2026-10-07-tag-pool]]), whose fix removes the full-pool
drop of write-behind data first.

## Rationale

Plan 9's answer keeps both properties #68 F1 traded between: the data is not
lost (the closer finishes the close) and the dying Proc is not held (a kill
hands the close away). A deadline guesses at a server's speed and drops data
when it guesses wrong; discarding loses data a server would have taken.
