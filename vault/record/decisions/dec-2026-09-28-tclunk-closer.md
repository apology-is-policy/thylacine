---
id: dec-2026-09-28-tclunk-closer
type: dec
title: "A dying thread's Tclunk is sent by a pool of closer threads"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-client, sub-kernel-ninep-session, sub-kernel-ninep-attach, sub-kernel-ninep-dev9p]
created: 2026-09-29
---
## Fork

A dying thread sends no request through the client's send path. Its Proc is
group-exiting, or a terminate note is pending, and `client_send_flow`
(`kernel/9p_client.c`) checks `client_self_dying()` at the top of its loop and
gives up before the first send. For most requests that is right: the thread is
unwinding, and nobody will read the reply. For a Tclunk it is a leak. The
client forgets the fid (the Tclunk's build unbinds it, and the never-sent tag
is reclaimed), but the server never hears of it, so the server's fid lives
until the session ends. On the root session that is until shutdown. On netd's
session the fid is a connection slot.

`dev9p_close` printed `9p: close: clunk of fid N refused rc 5`, and its
comment called the cause "the narrow burst-during-a-kill race". It is neither
narrow nor a burst. Each instrumented boot of 2026-09-28 printed the line three
times from one site. gopls kills a `go` child it has just spawned, while the
child is still in its kernel spawn thunk, before its first user instruction.
The thunk then drops the last reference to its exec Spoor
(`sys_spawn_full_argv_thunk`, `kernel/syscall.c`), and the clunk is refused.
The NP-4 audit (F1) found the same exposure for a socket's `ready` fid. The
other refusals in the same boots were on sessions whose server had gone. Those
fids die with their session and leak nothing, but the line did not tell the
two apart.

The at-exit handle drain is not exposed: it runs inside #68's exit-close
window, where `thread_die_pending()` answers false. The exposure is a last
reference dropped by a dying thread outside that window.

Two questions went to the operator. The first was who sends the Tclunk
(2026-09-28). The answer was a closer thread, so the second was whether that
is one thread or a pool (2026-09-29).

## Research

- **Plan 9** closes a dying process's channels on a kernel process. At exit,
  `closefgrp` hands each Chan to `ccloseq`, and `closeproc` runs the device's
  close, which for a mount is the Tclunk RPC (`port/chan.c`). An RPC made by
  the dying process would be cut short by its note, and a wedged server would
  hang the exit. `ccloseq` wakes an idle close proc and spawns another when
  none is idle: `if(!wakeup(&clunkq.r)) kproc("closeproc", closeproc, nil);`.
  A close proc that finds no work for 5 s exits. So a close blocked on one
  server never delays another. In 9front, a close proc that takes work spawns
  a spare when no other close proc is waiting.
- **Linux v9fs** retries the Tclunk once on `-ERESTARTSYS`, then leaks the fid
  until unmount.
- **Zircon** closes a dead process's handles in the kernel. The server sees
  `ZX_CHANNEL_PEER_CLOSED` on the file's own channel.
- **Mach and the Hurd**: the kernel destroys a dead task's send rights, and
  the server gets `MACH_NOTIFY_NO_SENDERS` on the open file's port.
- **Genode**: the parent closes a dead child's sessions.
- The principle they share: cleaning up after a dying client is never the
  dying client's job.
- **The tree**:
  - 9P multiplexes every fid over one session, the shape of Plan 9's devmnt,
    so there is no per-file channel for the kernel to close. The Tclunk has to
    go on the wire.
  - A session attached over a pipe (`SYS_ATTACH_9P`) has no receive deadline
    (`p9_client_recv_is_deadline_capable`). A thread that pumps it blocks
    until the server writes or dies, so one thread sending every session's
    Tclunks could be held by a single server that stops answering.
  - Boot kthreads exist: the poll pump, the weave reaper. A kernel thread
    cannot free itself, so a pool's retired threads are reaped by a peer, as
    Loom's SQPOLL thread is joined by `loom_free`.
  - NP-4b's `p9_session_retract_unsent` takes a never-sent op back whole,
    including a Tclunk's fid.
  - `p9_attached_ref` holds a session.

## Options

Who sends the Tclunk:

1. **A closer thread**, Plan 9's shape. A Tclunk that a dying thread cannot
   send is taken back whole and queued, with a session reference, to a kernel
   thread that is never dying. That thread sends it with ordinary
   back-pressure.
2. **Send if the ring has room**, the Linux shape: the dying check moves to
   just before a park. A Tclunk goes out whenever the ring has room, and leaks
   only under back-pressure. No new thread.
3. **Both**: inline when the ring has room, the closer otherwise.
4. **Widen #68's exit-close window**: a third setter of `exit_close_active`,
   around the last-reference clunks.

The closer's shape:

1. **A pool, one closer per session**, Plan 9's shape. A session with pending
   closes gets at most one closer. The closer that takes work spawns a spare,
   and idle spares retire.
2. **One thread, accepting the stall.** One stuck server holds every session's
   deferred closes until it answers or its Proc dies.
3. **One thread that never parks.** Each send is tried once, and a full ring
   or tag pool re-queues the entry on a timer. A pipe session whose tags are
   all held by unread replies would then need a receive deadline, a transport
   change.

## The call

Option 1 both times. The operator chose a closer thread on 2026-09-28, and a
pool with one closer per session on 2026-09-29.

- **The dying thread never waits.** Its hand-off is an enqueue that does not
  block (I-24).
- **Delivery is guaranteed while the session lives.** The queued entry holds a
  session reference, so the client outlives the entry. A session that dies
  first frees the fid with everything else, and the entry is dropped.
- **A stuck server holds only its own session's closer.** The pool has at most
  one thread per session with pending closes, plus one idle closer. A program
  that stalls many of its own servers at once costs a kernel thread for each,
  as in Plan 9.
- **The refusal line prints only while the session is live.** A clunk refused
  on a dead session leaks nothing. The line now names a real leak, and
  `tools/test.sh` fails on it.

Decided with it, as design points without a vote (`docs/FID-LIFECYCLE-DESIGN.md`
section 9):
- The take-back of a never-sent Tclunk cannot fail. Every outstanding request
  that may leave a fid bound when it ends holds a slot in the fid table. The
  shape question named 64 entries of headroom for this. A reservation gives
  the same guarantee without the extra entries, and also covers a walk's bind.
- flush(5) is kept on both sides. The client never flushes a Tclunk and
  honours a late reply to a flushed walk. Stratum's server sends an executed
  request's reply before its Rflush.

## Rationale

The closer is the heritage answer. Option 2 still leaks under back-pressure,
which is exactly when a close burst meets a kill. Option 3 delivers every
Tclunk too, but keeps two delivery paths to test and audit. Option 4 lets a
dying thread wait on a server, which is what the dying check exists to
prevent.

The pool follows from the closer. A thread that says the close on a client's
behalf inherits that client's server, and one wedged server must not strand
every other session's closes. Plan 9 answers with more close procs. One closer
per session bounds them by the work actually pending.

The capability microkernels close a dead client's objects in the kernel with
no message from the client. A 9P session has one channel for every file, so
the kernel's equivalent is a thread that says the close on the client's
behalf.
