---
id: dec-2026-10-06-loom-waiters-fan-in
type: dec
title: "A waiter reads for every 9P client it waits on, and only over a ready stream"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-loom, sub-kernel-ninep-client, sub-kernel-ninep-transport, sub-kernel-ninep-dev9p-poll]
created: 2026-10-06
---
## Fork

A Loom ring's ops can span 9P clients: an event loop over a socket and files is
the canonical use. Only a client's role holder reads its replies, and an async
op has no submitter thread to elect, so its reply is read by whoever holds the
role or by a waiter that reads for it. The waiters read for one client only.
The ENTER and the SQPOLL kthread pumped the client of the ring's newest
in-flight op, so a reply on any other client stayed unread while that one was
held or slow (OPEN-BUGS 2026-10-05 07:52Z, P2; a parked socket read never
answers). The ENTER's pump blocked in the recv whether or not anything was due
(the blind-recv residual (E), entrance (d)). The dev9p poll pump pumped at most
16 clients, collected from a LIFO head with no rotation, so a seventeenth
client's pollers hung (18:56Z, P3). The SQPOLL kthread and the poll pump
yield-spun while a foreign reader held the role (2026-09-30 15:04Z, P3). The
question: who reads a client's replies when no synchronous waiter does?

## Research

- **Plan 9.** `devmnt`'s `mountio` elects the waiting process as the reader
  (`m->rip`); a reply for another rpc wakes its owner, and the reader hands off
  when its own reply lands. There are no kernel reader threads, and no async
  mount I/O: concurrency is processes. Whoever waits reads.
- **Fuchsia.** Objects signal readiness to a port (`zx_object_wait_async`), and
  one thread fans in over many objects with `zx_port_wait`, then reads the
  ready ones. The waiter drives; the kernel only reports readiness.
- **io_uring.** A socket op that would block arms a poll on the socket's wait
  queue, and its wake queues task work that retries the op in the submitting
  task's context; with `IORING_SETUP_DEFER_TASKRUN` that work runs only when the
  task waits in `io_uring_enter`. The waiter drives, triggered by readiness.
- **Linux 9P (`net/9p/trans_fd.c`).** A poll callback (`p9_pollwake`) schedules
  `p9_poll_workfn`, and a read worker on the system workqueue reads frames for
  the connection whether or not anyone waits.
- **The tree.** srvconn already has readiness below the transport vtable
  (`srvconn_poll`, POLLIN = `s2c.count > 0`, its `poll_list` walked on every
  fill); the pipe transport's rx end has the pipe's poll. The death hangup
  already runs a backend op under `c->lock`, so the lock order a readiness
  sample needs exists. ARCH 21.10 chose the elected reader over a reader kthread
  because a kthread blocked in `recv` is the free-while-blocked hazard of #788
  and #713.

## Options

1. **Waiters fan in.** A mandatory transport op `recv_ready` samples "a recv
   would not block at a frame boundary" and registers a hook with the sample.
   Every waiter scans its in-flight clients and pumps one whose role is free
   and whose stream is ready; with nothing to pump it hooks each client (a held
   role on the role-waiter list, a free one on the readiness list) and sleeps
   on all the hooks. Plan 9's model, Fuchsia's port, io_uring's deferred task
   work.
2. **Async reader kthreads.** A reader per client (or a pool), woken by
   readiness, reads replies whether anyone waits. Linux's 9P transport. New
   kthread lifecycles on the deepest surface, the hazard ARCH 21.10 avoided.
3. **One client per ring.** Refuse a second client at register or submit. A
   restriction io_uring does not have; it fails the canonical event loop, and
   the poll pump would need its own fix.

## The call

Option 1, the operator's vote of 2026-10-06. LOOM.md 8.6 (the 2026-10-06
amendment), NET-DESIGN 12.2 (the poll-pump amendment) and ARCH 21.10 ("A waiter
with no reply of its own reads only over a ready stream") say so, and
`specs/loom_role.tla` models it over N clients.

## Rationale

A waiter that reads only over a ready stream never blocks at a frame boundary,
so it can wait on any number of clients at once, and nothing needs the recv
deadline the SQPOLL kthread and the poll pump used to come up for air. The
deadline, its gate on SQPOLL rings and the poll pump's 16-client cap all go.
Completions on a ring without SQPOLL still appear only when someone waits in
`ENTER` or a synchronous reader reads them, which LOOM.md already said; option 2
would have bought completions with nobody waiting at the price of kthreads that
block in `recv`. One mechanism now answers who reads a client's replies when no
synchronous waiter does, for all three waiters.
