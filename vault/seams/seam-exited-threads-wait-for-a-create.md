---
id: seam-exited-threads-wait-for-a-create
type: seam
title: "An exited thread is freed at its Proc's next thread create, not when it leaves its CPU"
status: open
surface: [sub-kernel-thread]
opened-by: chg-2026-10-20-in-proc-reap
tracker: "OPEN-BUGS 2026-10-20 (the multi-threading overhaul, chunk C1)"
created: 2026-10-20
updated: 2026-10-20
---
## Owed

A thread that exits while its peers live is freed by its Proc's next thread
create or Loom SQPOLL setup (`proc_reap_exited_threads`), or by `wait_pid` once
the whole Proc is dead -- not as soon as it has left its CPU, which is when
Plan 9 and Linux free it. Until then:

- **Size.** Each exited peer keeps its descriptor and its 32 KiB kstack.
- **Payer.** The Proc. The dead count against its own `PROC_THREAD_MAX`, so a
  user Proc holds at most 255 (about 8 MiB). A system-principal Proc, which the
  cap exempts, holds a whole burst: one that starts and joins 1000 workers and
  then creates nothing keeps about 32 MiB until it creates again or exits.
- **When it clears.** The next `SYS_THREAD_SPAWN`, vivarium clone or SQPOLL
  setup frees every exited peer that is off its CPU; one still on its CPU waits
  for the call after.
- **Behaviour that follows.** `proc_holds_tid_locked` (tkill, tgkill) finds an
  exited worker until a create frees it and answers ESRCH after, so the answer
  depends on whether a create ran since the exit.
- **Cost moved to the create.** One create frees every exited peer, each a
  guard restore with a system-wide TLB flush.

## What closes it

Freeing at the switch (overhaul chunk C1): the CPU that switches away from an
EXITING thread hands it to a reaper, or frees it once its run-queue lock drops,
with a kstack cache (chunk C2) taking the TLB flush off that path. Reaping at
thread exit instead first needs the two kernel tests that walk `p->threads`
without the lock (test_thread_spawn.c) moved under it.

## Risk while open

Memory only, and bounded per user Proc inside the I-32 floor; a burst is
unbounded only for the TCB, whose bursts are its own. No soundness risk: the
free uses the terminal predicate `wait_pid` frees on.
