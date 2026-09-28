---
id: dec-2026-09-28-poll-sample-arm-split
type: dec
title: "A poll samples remote readiness with a snapshot and arms only before it parks"
date: 2026-09-28
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-dev9p-poll, sub-kernel-poll, sub-netd-server, sub-ptyfs]
created: 2026-09-28
---
## Fork

A `poll()` over a `/net` socket or a pty learns readiness from the server that
holds it (netd, ptyfs). The kernel asked with one message: a readiness `Tread`
on the file's `ready` fid, its offset carrying the event mask, which the server
answers at once if the condition holds and otherwise holds until it does. That
one message did two jobs. It was the SAMPLE a poll's verdict rests on and the
ARM that wakes a parked poller, and a truthful "not ready" could not be said on
the wire at all. A poll that had to return (timeout 0, or a deadline lapsing)
read a cache of whatever the relay had delivered so far. The vivarium widened a
literal 0 to a 10 ms budget (`VIV_PPOLL_PROBE_MS`), and on 2026-09-28 that
budget failed the SMP gate under UBSan at `-smp 8`: a `ppoll(POLLOUT, 0)` on a
freshly accepted socket returned 0 (viv-pheno-probe leg L113).

The same root had two more consequences. A socket that was ready when `poll()`
was called went unreported beside a ready local fd, because the poll-pump
kthread collects a stranded probe before it pumps the probe's reply. That is
deterministic at `-smp 1`, where a syscall body is not preempted. And a cached
bitmap survived across calls, so it could report a level that a competing
reader had already lowered.

## Research

- **Plan 9** has no `poll()`. APE's `select` forks a read-ahead proc for each
  fd, so a zero timeout reads a local buffer and never asks a server. flush(5)
  says "the semantics of flush depends on messages arriving in order". A reply
  to a flushed tag that arrives before the `Rflush` must be honoured.
- **GNU Hurd** hit this bug in 2012. With a zero receive timeout on the client,
  the client gave up before the servers replied. Debian's first workaround was
  a 1 ms floor. The fix carries the deadline to the server
  (`io_select_timeout`), which checks readiness first and answers a deadline
  that has already passed at once.
- **QNX** `_IO_NOTIFY` has POLL and POLLARM. The reply always carries the
  current conditions, and POLLARM arms a later pulse only if none of them
  holds.
- **Fuchsia**: `poll()` waits on kernel objects whose signals the netstack
  asserts, so the poll path makes no RPC.
- **Genode**'s file-system session has the same flaw as ours. The
  `READ_READY` acknowledgement is held, and libc's `poll()` returns 0 on a zero
  timeout.
- **The tree**:
  - The 9P client discards the late reply of a flushed op by design
    (`kernel/9p_client.c`, `demux_orphan_late`).
  - Every Proc shares the `/net` session, with a pool of 64 tags.
  - netd can wake the kernel only through a held reply. The Weft direct-park
    wake is not wired.

## Options

1. **A SNAPSHOT bit in the readiness `Tread`'s offset**, answered at once and
   never deferred:
   - **(a)** as a fallback inside dev9p, used when its cache is empty;
   - **(b)** as the SAMPLE/ARM split. The snapshot is the only sample. The
     poll core settles every snapshot of a pass before it decides. The
     deferred read is only the arm, sent before a park. The cache is retired.
2. **`Tread` then `Tflush`**, with no protocol change: an `Rread` before the
   `Rflush` means ready. Rejected, because the client discards that reply by
   design. It also triples the frames and holds a tag of the shared pool until
   the `Rflush`.
3. **Push readiness words**: Weft-4's readiness word, for every connection.
   Rejected, because netd has no way to wake the kernel. Only the sample could
   be pushed, and the price is a server-writable page mapped into the kernel
   for each connection.

Two follow-on questions:

- **A server that never answers a snapshot**: a bounded and counted fail-safe,
  or wait without a bound like any other synchronous RPC.
- **The bound**: the call's own deadline, or a fixed interval that the call's
  timeout never cuts short.

## The call

Option 1(b), the SAMPLE/ARM split. The operator chose it on 2026-09-28, after
a Fable design review that the operator requested with the words "a more proper
systemic solution, not something bolted on".

- **The fail-safe is bounded and counted.** A snapshot still unanswered a fixed
  1 s after it was sent is flushed and reported not ready. The call's timeout
  never cuts that interval short. A counter records every expiry, the boot
  gates require it to stay zero, and a server that defers the snapshot must
  make it fire.
- **`VIV_PPOLL_PROBE_MS` is deleted.**
- **The vivarium's open of each socket's `ready` file on every call** costs
  three RPCs and a guest fd per socket per call. It is retired in its own
  sub-chunk after the merge.

## Rationale

The split gives each job its own message. That is the shape every peer arrived
at: QNX's POLL against POLLARM, and the Hurd's deadline that the server
evaluates. A poll's verdict is then always the server's own answer, given at an
instant inside the call, and never a cache. Settling all of a pass's snapshots
together costs one server round trip per pass, however many fds the pass holds.

A bound taken from the call's deadline would make a short-timeout poll give up
on a server that is healthy but busy, and guess. That is #98 again. So the
fail-safe is a fixed interval that only a server that has stopped answering
will reach. A poll may therefore return later than its timeout, by one round
trip. It never returns early on a guess.
