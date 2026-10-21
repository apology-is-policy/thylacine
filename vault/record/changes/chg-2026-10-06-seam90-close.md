---
id: chg-2026-10-06-seam90-close
type: chg
title: "A blocking 9P reader unwinds at any byte for a death, a stop or a caught note; the client keeps the partial frame"
date: 2026-10-07
arc: arc-boosty
commits: ["8672a3756", "d4c2f17a9", "cea674ef2", "ea18b94cd", "1c18fb87a"]
touched:
  - sub-kernel-ninep-client
  - sub-kernel-rendez
  - sub-kernel-notes
  - sub-kernel-thread
  - sub-kernel-loom
  - haz-shared-stream-desync
  - inv-i9
  - inv-i39
  - spec-reader-frame
  - spec-loom-role
established: []
closed:
  - seam-90-hung-server
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-07
---
ARCH 8.8.1.1 (2026-07-19, [[chg-2026-07-19-90-death-block-through]]) held the
elected 9P reader inside a frame: a death, a stop or a caught note unwound it
only at a frame boundary, because an unwind mid-frame lost the bytes read and
the next reader parsed the frame's tail as a header
([[haz-shared-stream-desync]]). Its cost was [[seam-90-hung-server]]: a server
that stops inside a frame holds its killed, stopped or signalled reader until
the server dies, and `SYS_ATTACH_9P` takes pipes from any process. Since
[[chg-2026-10-06-loom-multiclient]] the partial frame is the client's
(`c->rx_got`), so the stream no longer needs the rule. By the operator's two
votes ([[dec-2026-10-06-seam90-unwind-any-byte]]) the reader unwinds at any
byte for all three ([[sub-kernel-ninep-client]]: `reader_recv_frame` holds
`stop_unwinds` for the whole recv; [[sub-kernel-rendez]]: the die-checks and
the stop detour read no reader latch and `thread_reader_blocks_death` is
deleted; [[sub-kernel-notes]]: the caught-note claim no longer refuses a
mid-frame reader; [[sub-kernel-thread]]: the latches' meaning). Each event
takes the path it took at a boundary: a death the #845 abandon (a Tflush; the
tag reserved until the Rflush, I-10), a stop the role-free park and
re-election, a caught note flush(5). The premise -- every transport recv
returns the bytes it copied or none -- holds for all four recv implementations
(srvconn and the pipe read in production; loopback and mq never sleep).

**The model.** [[spec-reader-frame]] is rewritten: two readers, a server with
no fairness. TLC 2026-10-06 23:36Z, N = 3: clean 39 states (Safety +
EventuallyUnwinds), 39 under a fair server (+ FrameDelivered), and 34 for the
superseded rule under a fair server (the 2026-07-19 model's claim, a control).
`reader_frame_buggy.cfg` (an unwind that discards the partial frame) violates
NoDesync at 41; `reader_frame_blockthrough.cfg` (the superseded rule under a
server that stops) violates EventuallyUnwinds with Safety intact at 34 -- A two
chunks in, the server stopping, a stutter with A in its recv: the seam as a
counterexample. `specs/check-reader-frame.sh` pins all five counts.

**Tests.** `rendez.reader_recv_unwinds_death` (tsleep), `_death_sleep` (the
prompt path) and `_caught_note` replace the three #90 block-through tests;
`9p_srvconn_transport.reader_unwinds_mid_frame_death` and `_stop` drive a real
SrvConn whose server (the test) stops 20 bytes into a 160-byte Rgetattr. Each
was run RED: with `do_reader_recv_frame` discarding the partial frame on an
unwind, exactly the two transport tests fail on "the client kept the partial
frame"; with main's sched.c, 9p_client.c, thread.h and notes.c, exactly the
five new tests fail on their headline assertions and the boot completes. The
transport tests' first boot failed its premise -- the handshake's Tversion and
Tattach were still in c2s -- and the premise is now one assertion per setup
stage.

**Outside it.** A kernel thread (the SQPOLL kthread's reap clunk) and the
at-exit close (`exit_close_active`, #68 F1) still wait for a server: a killed
Proc whose reader left at once can still wait at exit when a close needs the
server. That is [[seam-close-flush-unbounded]], whose risk line assumed a
trusted server (corrected in its As-of note). Researching it found that a sync
9P op on a full tag pool fails EIO and a write-behind flush then drops its data
silently, confirmed by a witness run; both are owned (OPEN-BUGS); the
operator voted both designs on 2026-10-07, and they are the next two chunks.

**Also fixed.** `notes_deliver_tail` (static bool) fell off its end on the
native Plan 9 handler path -- the kernel's only -Wreturn-type warning,
pre-existing since bbc7ab90a -- while its caller loops on the value; it now
returns false as the Linux arm does, and `-Werror=return-type` joins the kernel
flags (a control build without the fix fails on exactly that warning).

**The audit.** Round 1 (Fable 5.1, read-and-reason on the unbuilt WIP): 0 P0 /
0 P1 / 0 P2 / 4 P3, all drift in scripture, the vault or the spec gate (the
I-9 row's validation cell, the seam note's status, unpinned counts and the
model's unnamed reading role, prose stating the old mechanism); a parallel
self-audit found three more of the same kind. Round 2 (Fable 5.1, focused on
the notes.c fix, the flag and the round-1 close): 0 / 0 / 0 / 3 -- the buggy
cfg's count unpinned, the TLC run's evidence uncited, and a RED-run checkout
that overlapped the read-only round. The dec note, already committed, omits
the seam from its `affects:`; this note's `closed:` carries the edge.

**The gates.** Suite 1904/1904 on cea674ef2 (seam90 alone, rebuilt after both
RED runs) and 1909/1909 on f8bd8688c (with aux-3 merged). ci-smp-gate N=10 PASS
on 1c18fb87a (default-smp1/4/8 + ubsan-smp4/8: 50 of 50 boots, 0 corruption;
2026-10-07 00:11Z-01:18Z); ls-ci PASS at 00:10Z (55 s, first attempt) on a
`--config ci` bake of ea18b94cd, whose code the tip carries unchanged.
