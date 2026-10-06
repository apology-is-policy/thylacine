---
id: dec-2026-10-06-9p-sessions-ends
type: dec
title: "A 9P connection's and session's counters are shown only to its two ends"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-devctl, sub-kernel-devsrv, sub-kernel-srvconn, sub-kernel-ninep-attach, sub-imperium]
created: 2026-10-06
---
## Fork

The CPU-time gate's audit ([[dec-2026-10-06-cpu-time-gate]], round 1 F4)
found that `/ctl/9p-sessions` published every connection's and session's
per-message counters to every reader: the ring byte counts, the server's frame
count, the demux counters, the reader flag, the send waiters and the in-flight
tags. A pty-served terminal carries one message per key, so another principal
could time a secret typed there. The trusted episode does not cross this file
(corvus reads the serial handle; the seat is a bare syscall), so IMPERIUM 11.3
item 10 held; the file needed its own rule, and a `/ctl` format change.

The first question offered: gate the counters to the system principal or a
hostowner (item 10's machine-counter rule), gate them per row to the row's
owner, or record the residual. The operator chose the first. That question
misstated its cost: it said the `#210` wedge probe, which reads the file as
michael, would have to elevate first. No login can. `CAP_HOSTOWNER` is
elevation-only, never clearance-grantable, and today only joey redeems it, at
boot provisioning. So the question was put again with the cost corrected:
under the system gate no login session ever reads any connection's counters,
its own included.

## Decision

The operator voted on 2026-10-06, on the corrected question: **per-row owner**.
A row's counters are shown to the principals at its two ends, and to a
`PRINCIPAL_SYSTEM` or `CAP_HOSTOWNER` reader; anyone else reads `-`.

Decided under the operator's "your guts" grant, as the vote's own scope:
- A connection's ends are the connecting Proc's principal, taken at the
  connect, and the poster's, taken at the post. Both are stored in the
  `SrvConn` by value, beside its stripes tags.
- A session's ends are its attaching Proc and, over a `/srv` connection, that
  connection's server.
- `PRINCIPAL_INVALID` marks an end the kernel does not know, such as the server
  behind a transport the caller supplied, and it matches no reader.
- The rows themselves stay world-readable: peer pid, label, msize, mode and
  state.

## Rationale

An end already sees every message on its row, so showing it the counts
discloses nothing it lacked. The shape is the one `cpu_ns` already has: a
per-object figure goes to its owner. Linux made the same call for
`/proc/<pid>/io` in 2011 (CVE-2011-2495: polling another user's I/O counts
gave their password's length). The system gate would
have closed the channel too, but it would also have taken from every user the
counters of their own connections, which the wedge autopsy needs. Recording
the residual left the channel open.

A reader running as `none` (PRINCIPAL_NONE) is no end either, decided under
the operator's grant at the audit close (round 1 F1): Procs that run as none
are unrelated -- a pre-auth server runs as none, one per remote client -- so
two of them sharing a principal must not share each other's cadence. The
/proc owner predicate (devproc_owner_or_hostowner: CPU time, sched, environ)
still treats none as one owner; that question is enqueued on its own.
