---
id: chg-2026-10-06-loom-multiclient
type: chg
title: "Waiters fan in: a Loom ring's waiters read every 9P client it has an op on, over a ready stream only"
date: 2026-10-06
arc: arc-boosty
commits: ["78d6714b9", "2faa703ed", "56f9c0270", "c6d9c76d4", "e1a15a777", "4fbe4caf5", "b65587a73", "b10dc12f6", "a16215e48"]
touched:
  - sub-kernel-ninep-transport
  - sub-kernel-ninep-client
  - sub-kernel-ninep-dev9p-poll
  - sub-kernel-loom
  - sub-kernel-srvconn
  - sub-kernel-pipe
  - sub-kernel-rendez
  - spec-loom-role
established: []
closed:
  - seam-221-idle-pump-wake
  - seam-223-pump-tail-starvation
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-06
---
A Loom ring may hold registered handles on many 9P sessions, but its waiters
read only one of them. The non-SQPOLL ENTER pumped the client of the newest op
in flight, so an op on any other client completed only if something else
happened to read that client (OPEN-BUGS 2026-10-05 07:52Z, P2). The SQPOLL
kthread pumped the same single client with a 10 ms recv deadline and spun on
`sched()` while another thread held its reader role (09-30 15:04Z). The dev9p
poll kthread pumped at most 16 QTPOLL clients from the head of a LIFO list, so
a seventeenth starved outright ([[seam-223-pump-tail-starvation]]), and it
re-polled every 20 ms while a probe was parked ([[seam-221-idle-pump-wake]]).
Every pump also took the reader role and blocked in the transport recv with no
bytes waiting, blind to client-side progress (09-30 11:01Z, the ENTER half).

The operator voted (2026-10-06) that the waiters fan in. Scripture first
(78d6714b9: LOOM.md 8.6, ARCH 21.10), then the build:

- **Transport readiness and a read that never sleeps**
  ([[sub-kernel-ninep-transport]], [[sub-kernel-srvconn]], [[sub-kernel-pipe]]).
  Two mandatory vtable ops. `recv_ready(ctx, pw)`: a recv would not block
  (bytes, EOF or an error), with `pw` registered on the backend's readiness
  list atomically with the sample. `recv_now(ctx, buf, cap)`: read what is
  waiting and never sleep (`P9_TRANSPORT_EAGAIN` when nothing is). srvconn
  answers both from its s2c ring; the spoor transport from the pipe's `poll`
  and `pipe_read_now` (which leaves EL0's own `O_NONBLOCK` alone) and refuses a
  non-pipe rx; the loopback and mq test transports from their stage. The
  recv-deadline machinery (`set_recv_deadline`, `recv_timed_out`, the deadline
  pump and the SQPOLL register gate) is deleted, so a spoor-transport session
  can now ride an SQPOLL ring: a strict widening.
- **The client** ([[sub-kernel-ninep-client]]). `p9_client_reader_pump_ready`
  takes the role only when it is free and the stream ready, reads with
  `recv_now`, and demuxes a whole frame (DEAD / IDLE / BUSY / PROGRESS). A frame
  found in part stays with the client (`c->rx_got`), as Plan 9 devmnt keeps it
  in the mount's queue and Linux trans_fd in the connection; every reader
  resumes there. So no pump ever waits on a server: one kthread pumps every
  QTPOLL session in the system, and any process can serve a 9P mount over pipes
  and keep the read end. `p9_client_reader_hook` files one hook per client: on
  the role list while the role is held, on the readiness list while it is free
  (hooking readiness under a held role would miss a holder that leaves bytes
  behind). A dead session, or one whose transport is closed, pumps DEAD and
  refuses a hook.
- **The fan-in set** ([[sub-kernel-loom]]). Up to 64 distinct in-flight
  clients, each pinned, scanned from a rotating cursor. A waiter pumps every
  ready client; with nothing pumpable it hooks them all and the CQ, and sleeps
  on one Rendez. A partial set (more than 64 clients, possible only across a
  re-register) rescans every 10 ms. The ENTER and the SQPOLL kthread share it.
  The kthread parks with ops in flight on what moves (a hook flag, the CQ flag,
  an SQE produced, a CQE reaped, stop) rather than on a deadline.
- **drive_gen.** A completion read by another thread posts its CQE before it
  records a re-arm or a chain result, and a sibling thread's submit can put an
  op on a client the waiter has not hooked. Every such change bumps
  `drive_gen` under `l->lock` (a CQE post, a completion's state update, an op
  linked in flight, a re-arm claimed), and a waiter sleeps only if it has not
  moved since its loop top.
- **The dev9p poll kthread** ([[sub-kernel-ninep-dev9p-poll]]). It collects
  every client with a read out (no cap), holds a session ref across pump, hook
  and park, and parks until a kick or a hook flag. The 20 ms timer remains only
  as the GC backstop while an arm is linked, which is the close
  [[seam-221-idle-pump-wake]] asked for; the pump itself waits on the
  transport. Both seams close here.

[[spec-loom-role]] was generalised to many clients (`Clients`, `NSYNC`,
`Deferred`), the old blind-recv residual became the buggy cfg
`BUGGY_UNREADY_PUMP`, and four more buggy cfgs pin the fan-in's rules. The
two-client cfg ran bounded (79,010,570 distinct states, depth 28, no
violation). drive_gen and the partial frame lie below the model's grain; their
witnesses are tests.

**The audit.** Round 1 (Fable 5.1) found 0 P0 / 0 P1 / 0 P2 / 3 P3: the fan-in
frame's kernel-stack depth unmeasured (now watermark asserts), a backstop it
withdrew (the park wakes on a reap), and a stale doc. The self-audit beside it
found three P1s the round missed. S-3 was the chunk's own: dropping the deadline
gate made pipe-served QTPOLL files remote, so the one dev9p poll kthread read
streams any process serves, and a reply's header followed by silence would have
hung every /net poller in the system. Its fix is `recv_now` and the client's
partial frame above. S-5 was in that fix: the non-sleeping read set the
stop-unwind latch and left it set, one ^Z from an EXTINCTION in a death-only
sleep. S-1: a sibling thread's submit could slip between a waiter's hooks and
its CQ hook (the link bumps above). Round 2 (Fable 5.1) on the fixes: 0 P0 /
0 P1 / 0 P2 / 4 P3, all closed -- a closed transport now pumps DEAD (it sampled
ready while `recv_now` found nothing), the spoor `recv_now` refuses a non-pipe
rx, the 1 KiB syscall-entry allowance is measured (416 B), and the comments that
still cited a stream desync as the reason for #90's mid-frame block-through now
call it what it has become: a voted policy whose cost is
[[seam-90-hung-server]] (corrected here: any process can serve a 9P mount over
pipes). Closing that seam, by letting a death or stop unwind at any byte, is the
operator's decision.

**The first boot** found three test defects, none in the kernel: a test put 16
KiB of mock pipes on the boot stack, one misread `p9_transport_send`'s return,
and `loom_quiesce_abandons_inflight` had accepted a session death as success, so
the #898 late-reply-after-abandon path was never exercised; it now runs over the
mq FIFO (`loom_quiesce_drains_the_late_reply`).

**Each new witness, proven RED.** The chunk's eighteen new or tightened tests
were each run against a sabotage of the code it guards, in eight groups whose
paths and targets do not overlap, every group rebuilt before its boot: each
failed on its named assertion, and each passes on the real code. One sabotage
taught something. A reader that never resumes a partial frame left
`pump_ready_chunked_frame_completes` green, because that test's frame arrives
within one pump; the resume is witnessed by
`pump_ready_never_waits_inside_a_frame`, and a second sabotage (one chunk per
pump) turned the chunked test red as well.

**The spec checkers.** A checker that pinned the state count of a cfg violating
a temporal property was pinning a timing: TLC checks liveness at time-triggered
points during the run and stops at the first violation, so one cfg gave 32,796
states on one quiet run and 32,868 on the next. With `-lncheck final`
(a16215e48) TLC checks liveness once, over the whole space, and the counts
repeat; check-loom-role, -debug-stop, -cow and -tail-order pin those counts
now.
