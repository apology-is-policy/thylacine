---
id: sub-kernel-ninep-client
type: sub
title: "The 9P client (shared elected-reader core)"
parent: moc-kernel-ninep
code:
  - kernel/9p_client.c
  - kernel/9p_session.c
  - kernel/9p_transport.c
  - kernel/9p_srvconn_transport.c
  - kernel/9p_transport_mq.c
  - kernel/9p_attach.c
  - kernel/include/thylacine/9p_client.h
audit: hard
guarded-by: [inv-i9, inv-i10, inv-i11]
validated-by: [spec-9p-client, spec-reader-frame, gate-smp]
locks: [lock-9p-client-c-lock]
hazards: [haz-shared-stream-desync, haz-single-waiter-rendez, haz-death-path-wake]
abis: []
design: ["docs/ARCHITECTURE.md sections 21 + 21.10 + 8.8.1.1"]
created: 2026-07-31
updated: 2026-10-06
---
## Purpose

The kernel's 9P2000.L client: consolidates the wire codec, the session state
machine, and the transport byte-pipes into one op-per-function API
(`p9_client_*`), and is the single object a dev9p mount hands to EVERY Proc
whose territory resolves through it. It is therefore not a per-caller
convenience but a **multi-Proc-shared, internally-locked, pipelined**
component: the SYSTEM Stratum mount, the per-user home mounts, corvus, and
netd's `/net` each ride one instance, concurrently, from different CPUs.
That sharing is what makes every design decision here a whole-system
availability decision ([[lin-9p-client]] lesson 1).

Layering: `syscall / dev9p` → **p9_client** → `p9_session` (state machine) +
`p9_transport` (byte pipe: srvconn, spoor, loopback, mq-test) → `p9_wire`
(codec).

## Contract

One function per op, `0` on success / `-errno` on failure:

- **Lifecycle**: `p9_client_init` (caller provides the recv buffer sized to
  msize; the struct is ~36 KiB — never stack-allocate), `p9_client_destroy`,
  `p9_client_close`.
- **Handshake**: `p9_client_handshake` (Tversion + Tattach; runs on a
  still-private client with `HANDSHAKE_DEADLINE`, then steady state blocks
  with no per-op deadline — death-interruptible instead).
- **Path**: `p9_client_walk` / `walk_one` / `walkgetattr` (POUNCE fused) /
  `clunk` / `clunk_async` (fire-and-forget; ownerless Rclunk drain). Both
  drain a full tag pool before the build, and both return `-P9_E_AGAIN` when
  the Tclunk could not be sent on a live session (a dying caller, or a spill
  or reply buffer that could not be allocated): nothing reached the wire, the
  tag is free and the fid is STILL BOUND, so the caller hands it to the closer
  ([[sub-kernel-ninep-attach]]).
  `p9_client_fid_held(c, fid)` (live, OPEN, bound) is the leak test a failed
  clunk is judged by; `p9_client_set_orphan_sink` installs where an
  ownerless late walk reply's fid goes.
- **I/O**: `lopen` / `lcreate` / `read` / `write` — reads and writes clamp a
  single op to the negotiated msize payload and return SHORT
  (`client_max_read_count` = msize − 11; `client_max_write_payload` =
  `min(msize, out_buf_cap)` − 23); callers loop per POSIX short-op
  discipline.
- **Metadata / mutation**: `getattr` / `setattr` / `readdir` / `statfs` /
  `fsync` / `symlink` / `mknod` / `rename` / `readlink` / `link` / `mkdir` /
  `renameat` / `unlinkat`.
- **Weft**: `p9_client_weft` (Tweft → share_id + ring geometry) /
  `p9_client_weftio` (the zero-copy data drive).
- **Async front-end** (the Loom completion seam): `p9_client_submit_async`
  (`p9_rpc.on_complete` = WAKE_RENDEZ vs POST_CQE), `p9_client_reader_pump_once`,
  `p9_client_reader_pump_once_deadline` (the SQPOLL idle pump — deadline
  armed on only the FIRST recv, the frame boundary, so a timeout consumes no
  bytes; returns PROGRESS/IDLE/BUSY/DEAD), `p9_client_handoff_reader`,
  `p9_client_abandon_async`, and `p9_client_role_wait_register` /
  `_unregister` (hook a `poll_waiter` that a free, undesignated reader role or
  the session's death wakes: the Loom ENTER's wait for the role; dead ->
  `-EIO`, role free -> 0, hooked -> 1).

Error convention: `-EINVAL` bad args/magic · `-EBUSY` not-OPEN · `-EIO`
lower-layer failure · `-<ecode>` the server's Rlerror ecode, **bounded to
`[1,4095]` (Linux `MAX_ERRNO`) before negation** and otherwise collapsed to
`-EIO` (`map_error`); callers of Stratum-extension surfaces may still need
to translate the in-range `STM_E*` codes. `-P9_E_INTR` (EINTR) means a caught
note interrupted the op AND the server's `Rflush` confirmed it cancelled, so
the op had no effect (flush(5)); an op whose reply beat that `Rflush` returns
its result instead.

## Mechanism

**Elected-reader pipelining** (Plan 9 `devmnt`/`mountio`). Each op allocates
a stack `struct p9_rpc`, registers it in the tag-indexed `c->inflight[]`
under `c->lock`, sends its frame, then enters `client_wait`: a submitter
with no reply yet becomes THE reader (one at a time via `c->reader_active`),
drops the lock, `reader_recv_frame`s one frame, retakes the lock, demuxes it
by tag to the owning rpc (frame copied to that rpc's `reply_buf`, waker
wakes its own rendez), and repeats until its own reply lands; everyone else
sleeps on their OWN rpc rendez. A departing reader hands the role off
(`client_handoff_reader_locked`) to one still-pending rpc — skipping
an rpc whose thread is parked for a stop (`rpc->stop_parked`, which the thread
sets itself in `client_debug_stop_park` under `c->lock` for exactly the park's
span; the Proc's stop flags are not read, because a resume-then-re-stop flips
them while the thread never runs) so the role lands on a runnable survivor, and
skipping an rpc still `sending` (registered, but its thread is still getting a
frame onto the wire: the #349 send park, or a flush's staging). A dying thread
is never stop-parked: `client_stop_pending` asks `proc_stop_requested`, which
answers false once the group's exit message is published, so it does not park,
and a park its group's death finds ends at once and clears the flag. Its op
takes the role like any other: `client_wait` bounces the role on if the thread
is dying outside its exit close, and a closer in its exit close keeps it and
reads its own reply (DEBUG-FS-DESIGN 5g). The handoff sets `be_reader` as a
pure advisory wake-hint (election
is gated solely by `reader_active` under the lock, so two readers are
impossible regardless of how many carry the hint). A sending thread sleeps on
the send list, where neither the handoff's wake nor its `be_reader` check
reaches it, so a designation there waited for an unrelated wake; and a death
or a stop there (including the stop detour inside that sleep) took the role
with it, leaving a survivor in `client_wait` with no reader (flush(5) round 2
F2, 2026-09-30; the #349 park had it before the flush staging loop copied it).
It needs no designation: every departure signals the send list first, and a
woken sender self-elects -- except a tag drainer (the flush staging and
`client_drain_until_free_tag`), which re-parks while a tag is owed and then
rests on that owner's dispatch, which signals the send list when it frees the
tag. `client_wait` clears `sending` on entry, so every
rpc a designation can reach is one that can act on it.

**Send-side flow control.** A transiently-full c2s ring is back-pressure,
never death: `srvconn_transport_send` returns `P9_TRANSPORT_EAGAIN` at
`n==0`, propagated by `do_send` only at `sent==0` (all-or-nothing — zero
bytes on the wire). `client_send_flow` then: (1) **spills** the built frame
out of the shared `out_buf` into a private kmalloc copy at the FIRST EAGAIN
— `out_buf` is undefined across any lock drop ([[lock-9p-client-c-lock]]);
(2) if no reader is active, **self-pumps** one s2c frame (draining replies
frees the server to drain c2s — the deadlock-breaker; its own tag is not on
the wire, so it can only demux OTHER ops); (3) else **parks** on
`c->send_waiters_list` — a multi-waiter `poll_waiter_list`, each sender on
its own stack rendez ([[haz-single-waiter-rendez]]) — until
`client_send_progress_signal` (fired per demux and on reader departure) or
death; then retries from the spill. Never-sent exits (`CLIENT_SEND_NEVER`:
self-dying, dead-observed, spill-OOM) reclaim their tag immediately via
`p9_session_abort_unsent` — zero bytes reached the wire, so I-10-safe —
except a Tclunk on a live session, which is taken back whole (below). A
Tflush has one more never-sent exit, its op's own reply landing while it waits
for ring space; `p9_session_flush_retract` reclaims that one (the flush(5)
section).

**A Tclunk the caller cannot send leaves its fid bound (2026-09-29,
FID-LIFECYCLE section 9).** A dying thread cannot send (`client_send_flow`
refuses it at its loop top). Before, its Tclunk's build unbound the fid, the
send was refused, and the server kept the fid until the session ended --
gopls's kill of a `go` child in its spawn thunk did it three times a boot.
Now the clunk refuses a caller already dying before it builds anything;
`client_drain_until_free_tag` checks death before the free tag (a dying
caller builds nothing); and a built Tclunk that never reached the wire -- the
caller died while parked on back-pressure, or its spill buffer could not be
allocated -- is taken back with `p9_session_retract_unsent`, which re-binds
the fid into the slot the Tclunk kept ([[sub-kernel-ninep-session]]). Each
returns `-P9_E_AGAIN`; every other op keeps `-P9_E_IO`, since `P9_E_AGAIN` is
EAGAIN and a read must never surface it for a spill-OOM. A dead session keeps
`-P9_E_IO`: its fids died with it. Tests, each seen red by a sabotage:
`9p_client.clunk_dying_keeps_fid_bound`, `.clunk_killed_while_parked`,
`.clunk_killed_in_tag_drain`, `.clunk_dying_waiter_sends_no_flush`,
`.flushed_walk_late_reply_to_sink`, `.abandoned_walk_late_reply_kept`,
`.abandoned_async_clunk_not_flushed`, `.clunk_rlerror_drains_as_clunk`, and
the fail-closed trio `.clunk_malformed_reply_fails_closed`,
`.abandoned_walk_malformed_late_reply_fails_closed` and
`.flush_malformed_reply_fails_closed` (the recording responder answers one
T-type with a 9-byte reply no parser accepts); the dying thread is
`test_dying` (kernel/test/test.c), killed the way the group-terminate cascade
kills each peer.

**An async submit cannot wait, so a shortage is its retryable error (NP-4b,
2026-09-28).** `p9_client_submit_async` checks for a free tag BEFORE it
builds, and on a send that meets a full ring (`P9_TRANSPORT_EAGAIN`) it
clears `inflight[tag]` and takes the op back whole with
`p9_session_retract_unsent` (the tag, and the fid a Tclunk unbound at
build). Either way the op completes with `-P9_E_AGAIN` (== T_E_AGAIN), and
the shared session stays live. Before, a full ring latched the WHOLE session
dead (every op of every Proc on the mount failed) and a full pool read as
`-EIO`, which dev9p's poll reported as a socket error. A resubmitted op goes
out (`9p_client.async_send_eagain_keeps_session_alive`,
`9p_client.async_full_tag_pool_is_eagain`, both seen red first). The sync
front-end still answers a full tag pool with `-EIO` (OPEN-BUGS: the pool is
shared with poll arms held until readiness), except the sync clunk, which
drains a tag first as the async one does.

**Abandon on death.** A Proc dying mid-op NULLs `inflight[tag]`, frees its
reply_buf, and sends `Tflush(oldtag)`; the tag stays reserved
(`awaiting_flush`) until its Rflush — never freed by a late original reply.
A Tclunk is never flushed (flush(5)): a flush the server honours would cancel
the clunk, and the fid, unbound at the build, would stay live on the server
with nobody to clunk it. A sync clunk whose waiter dies or is interrupted
leaves the Tclunk in flight without an owner, like an async one, and returns
0; `p9_client_abandon_async` leaves an async Tclunk the same way. A flushed or
abandoned WALK's late reply binds its new fid (the session's
`honour_late_walk`, or the normal arm for an abandoned tag); the demux hands
that fid to the orphan sink, only when the dispatch succeeded (`client_orphan_fid_locked`: `orphan_handed`, or
`orphan_kept` when there is no sink or it cannot take the fid, and the fid
then dies with the session). The `9p: op abandoned` line says which of
`flush sent` / `flush rolled back` / `flush send failed` / `no flush staged`
happened; before 2026-09-29 a rolled-back flush read "flush sent".
The flush sends are EAGAIN-aware WITHOUT pumping (a dying thread must not
park): on EAGAIN or a failed build, `p9_session_flush_rollback` /
`p9_session_mark_abandoned` fall back to the ownerless reclaim (the
`abandoned` bit: the late original reply frees the tag; the victim is
excluded from `any_outstanding_on_fid` so a cancel-then-close Tclunk still
sends). Only a genuine transport break latches the session.

**A caught note flushes and waits (flush(5), 2026-09-30; ARCH 8.8.3 and
21.10).** A caught note that interrupts a Linux-phenotype op's wait
(`CLIENT_WAIT_NOTEINTR`, 11b-9p) leaves the thread alive, so the death abandon
above is wrong for it: flush(5) says a reply that arrives before the Rflush
must be honoured. From 11b-9p until this fix, an interrupted socket or pts
read lost the bytes the server had consumed, and a write that had completed
reported EINTR. `client_flush_wait` now marks the rpc `noted`: the note stays
pending until the EL0-return tail, so every later wait for the op is killable
only (`sleep`, and `reader_recv_frame` with `caught_ok=false`), or it would
only interrupt again (the claim is the thread's to re-take). The rpc KEEPS
`inflight[tag]`. It stages the Tflush -- with a full pool it makes one unit of
progress at a time and re-checks its own reply after each, because this op is
on the wire and a pump can demux its answer, so `client_drain_until_free_tag`,
which waits only for a free tag, would read on past it. A unit is a pump
(`client_pump_or_park_locked`), except while a tag is owed
(`client_tag_owed_locked`: a sync op's reply is stored and its owner, not
parked for a stop (`stop_parked`), has yet to run the dispatch that frees the
tag): then it parks
for that dispatch's signal, because no frame announces the freed tag and a
second pump would wait for an unrelated reply (flush(5) round 3 F3; the async
clunk's drain does the same). The staged Tflush marks the op `owner_waits` in
the session and goes out through `client_send_flow`, parking on back-pressure
like any send -- but that loop stops at the op's own reply (`rpc->noted &&
rpc->done`, below). Then it sets `flushing` and waits in `client_wait` for the
first answer:
- **The original reply first.** The demux applies it at once, in wire order,
  with `client_honour_locked` -> `p9_session_dispatch_flushed_rmsg`: the whole
  reply, fid state included, into the caller's `out`, with the tag still
  reserved until the Rflush (I-10 unchanged). It drops the registration and
  sets `honoured`. The call returns its result, and the Rflush drains ownerless
  later (`demux_orphan_flush`). A reply that lands before the Tflush is on the
  wire is only stored (`flushing` is not yet set), and it answers the op
  outright: the Tflush goes back unsent (the retract below) and the reply
  completes the call as an ordinary one.
- **The Rflush first.** The orphan-flush arm reads the flush's `flush_oldtag`
  before dispatching; once the dispatch has freed both tags, it drops the
  still-registered owner's `inflight[oldtag]` in the same critical section, so
  the tag cannot be reused under it, and sets `flushed`. The call returns
  `-P9_E_INTR` (`CLIENT_WAIT_FLUSHED`). A registered owner whose Tflush is not
  yet on the wire means the server answered a flush it never got: fail closed.
- **A death in the flush wait** drops the registration and returns `-P9_E_IO`
  (`9p: op abandoned (tag N, death, in its flush wait)`); both frames drain
  ownerless, as after the #845 abandon. The arm also clears `owner_waits`, so
  the dead owner's fid goes to the closer before the Rflush lands. The #845 arm
  cannot do that: its second `send_flush` is refused on an `awaiting_flush`
  tag and `mark_abandoned` skips one, so falling through would leave the fid
  held until the Rflush and refuse the closer's clunk (#294). The two arms
  must stay separate.
- **A Tflush that never reaches the wire** (`CLIENT_SEND_NEVER`: the op's own
  reply came first, or the sender could not wait) is undone with
  `p9_session_flush_retract` (not `flush_rollback`: the owner is still here, so
  the op stays live), and the op waits for its reply, killable only -- or has
  it already, and completes with it; either way the note delivers after the
  call. A genuine send break latches the session dead; no reply of the op's can
  be stored by then, because the send loop re-checks under the lock it sends
  under.
While the owner waits, the session counts its flushed op LIVE for the fid
exclusion (`owner_waits`, set when the Tflush is staged): a reply that beats
the Rflush is applied in full, so the op may yet act on its fid, and a clunk
of that fid is refused. The honour clears the bit (the op has acted), and so
does a death in the flush wait (the fid is the closer's, before the Rflush);
the #845 abandon never sets it (flush(5) round 3 F4).
`c->flush_honoured` / `c->flush_cancelled` count the two outcomes. A Tclunk is
never flushed, here as on death. The orphan-flush arm fails the session closed
on a registered victim that is async or not yet `flushing`: every other flusher
drops its registration before its Tflush, so only a server answering a flush
it never received can produce one. Tests, each seen RED on the pre-fix client:
`9p_client.note_flush_honours_late_read`, `.note_flush_rflush_first_cancels`
(the read's fid is not clunked while its owner waits, and clunks once the
Rflush has cancelled the read),
`.note_flush_death_abandons` (whose fid the closer can clunk before the
Rflush) and `.note_flush_reader_honours_walk` (the flush
wait holds the reader role and honours its own Rwalk: the caller gets the fid,
not the closer), `.note_flush_full_pool_own_reply` (a full pool: the pump for a
tag demuxes the op's own reply, and the op stops there) and
`.note_flush_pump_wakes_parked_flush` (below), `.note_flush_reader_rflush_first`
(the flush wait holds the reader role and reads its own Rflush: `-EINTR`, and
it stops reading there) and `.note_flush_reply_beats_unsent_flush` (the
Tflush meets a full send ring; the op's own reply, demuxed while it waits,
completes the call and the Tflush goes back unsent),
`.note_flush_handoff_skips_staging` and `.handoff_skips_send_parked` (a
departing reader does not designate the op parked on the send list with the
lower tag), `.note_flush_staging_waits_for_owed_tag` and
`.async_clunk_drain_waits_for_owed_tag` (a drainer whose pump completed a sync
op waits for that op's dispatch, not a second frame; on the mq loopback a
second read is an EOF that kills the session), and
`9p_session.flush_owner_waits_keeps_fid_live`. Each mechanism has a sabotage
that turns its test RED. They run a Linux-phenotype
`test_dying` thread whose SIGCHLD is caught and post a real child_exit through
the exit path.

**Every freed tag, and every pump's departure, signals send progress
(2026-09-30, the flush(5) round-1 F1).** A sender parked in
`client_pump_or_park_locked` -- on back-pressure, or draining a full tag pool --
sleeps on the multi-waiter send list and wakes only on
`client_send_progress_signal` or death. Before, only `client_wait`'s reader (per
demux, and on departure) and the self-pump signalled. So a tag freed by an
owner's DONE dispatch, a never-sent take-back, a Tflush roll-back (the death
abandon's or `p9_client_abandon_async`'s) or retract, or an async retract woke
no drainer, and a drainer parked on a full pool could
sleep on with a free tag in the table. Worse, `p9_client_reader_pump_once` and
`_deadline` (the SQPOLL and dev9p-poll kthreads) departed without signalling,
so every sender that parked while a pump held the role slept until an
unrelated op arrived. Each of those sites now signals:
`client_take_back_unsent_locked` holds the never-sent reclaims (both
`client_run` sites and `p9_client_clunk_async`'s). It must run under c->lock,
and asserts it: `CLIENT_UNLOCK_RET` evaluates its value after the unlock, so a
take-back written as its argument ran unlocked, racing peers over the tag and
fid tables (flush(5) round 2 F1, a P1 in the round-1 fix; the async clunk now
takes back before the unlock). The regressions are
`.note_flush_pump_wakes_parked_flush`, a flush-stager parked on a full pool,
and `.note_flush_reply_beats_unsent_flush`, a Tflush parked on a full send ring
(the ordinary #349 park): each is woken only by the departure of the pump that
demuxed its op's own reply.

**Frame-atomic recv.** `reader_recv_frame` (thin wrapper over
`do_reader_recv_frame`) holds `stop_no_park` for the whole recv tenure and
sets `stop_unwinds = (got == 0)` per-chunk: a death OR a debug/job stop
unwinds the reader ONLY at a frame boundary and BLOCKS THROUGH mid-frame
(the die-check sites in `sleep()`/`tsleep()` are guarded by
`thread_reader_blocks_death`), because delivery is CHUNKED and a mid-frame
unwind desyncs the shared stream ([[haz-shared-stream-desync]]). A
boundary stop-unwind is classified via the stable per-Thread `stop_unwound`
latch (set by the detour, reset at recv entry, read by the same thread) —
never by re-reading `debug_stop_req`, which an async resume can clear. A
stop never marks the session dead; death always wins over a stop at every
branch.

**Fail-close.** `client_mark_dead_locked` is the SOLE `c->dead` setter
(transport EOF/error, or a demux-level protocol violation — malformed
header, out-of-range tag, oversize); it fails every in-flight rpc and
wakes both the per-rpc rendezes and the parked-sender list. A dead session
rejects all subsequent ops; there is no reconnect (destroy + re-init above).
The sync (WAKE_RENDEZ) front-end and the boot path fail every op `-EIO`;
the *async* (POST_CQE / Loom) path carries one more distinction (below).

**A death hangs up (ARCH 21.10, 2026-10-05).** On the false-to-true edge of
`c->dead`, and only there, `client_mark_dead_locked` calls
`p9_transport_hangup` under `c->lock`, so the server learns of the death now
rather than at the mount's last close: a pipe-mounted server (haul, a posted
srv) reads EOF once it has drained what was sent, and a srvconn server's
worker leaves its serve loop ([[sub-kernel-ninep-transport]]). Before, a
session the kernel had killed -- a reply with a tag it never issued, an
oversize frame -- left the server serving a dead mount that told no one. Every
later call finds the session already dead, so the hangup runs once. The NOTAG
version exchange in `client_run` is not a death: a refused Rversion fails the
attach through `map_error` before there is a session to kill, and hangs up
nothing. Witness: `9p_client.death_hangs_up_once` (two deaths, one hangup;
then the dead session's close reaches the transport and is not counted as a
hangup).

**The device-gone death reason ([[inv-i29]] device-gone extension, Menagerie
step 4).** `client_mark_dead_locked(c, bool devgone)` takes a reason, and the
three reader sites (`client_wait`'s elected-reader loop and the two
`p9_client_reader_pump_once*`) pass `rr == 0`: a **clean EOF** (`recv` returned
0 — the server/driver endpoint torn down) maps a dying session's async ops to
the device-gone `-T_E_NODEV` (ENODEV), while a `recv` error / armed-deadline /
malformed frame (`-1`) keeps the transport `-T_E_IO`. Before step 4 both
collapsed to `-1`. So a driver group-terminated by a `DeviceRemoved` tears down
its served endpoint → the consumer's rings EOF → its reader sees `recv 0` → its
in-flight Loom ops complete `-ENODEV`, the whole chain automatic with **no
warden code on the consumer's client**. `p9_client_mark_devgone(c)` is the
explicit secondary entry (a device-teardown hook that holds the client),
idempotent — the first death's reason stands. The reason rides only the async
path because it is a Loom-completion (I-29) property the sync ABI does not
expose; the audited #841 synchronous surface is untouched. Exactly-once holds
by the demux clearing `inflight[tag]` **before** completing, so a reply and a
death never both terminate one op — a late reply on a death-completed op
dispatches ownerless (the `demux_orphan_late` taxonomy below) and is
discarded, never a second terminal CQE. Spec: `loom_devgone.tla`
(`NoDoubleTerminal` / `DeathResultFaithful` / `SessionDeathCompletes`).

**Buffers.** Tmsgs build in the two-tier `out_buf` (inline 32 KiB, or an
msize-sized kmalloc for a `DMSRVBULK` 128-KiB session; OOM degrades to
inline — shorter writes, still correct). The read/readdir/readlink dispatch
results zero-copy alias the per-op `reply_buf`; `client_run` keeps that
buffer alive past return via the single `c->done_reply_buf` slot (freed at
the next completion or destroy, under the lock) so the public op's copy-out
is valid.

**The demux counter suite + the ownerless taxonomy** (#210).
`demux_frame_locked` is the sole mutation site for six per-client
counters, all under `c->lock`: `frames_rx` (every steady-state frame that
reached the demux), `demux_owned` / `demux_wakes` (frames with a live
`inflight[tag]` submitter, and sync wakeups actually issued), and a
**three-way split of the ownerless case**.

The split is the whole point, and it encodes the #214-F1 conflation
lesson: "ownerless" is not one pathology, it is one pathology wearing
three by-design flows as camouflage.

| Counter | Why a frame legitimately arrives unowned |
|---|---|
| `demux_orphan_clunk` | `p9_client_clunk_async` never registers `inflight[tag]`, so **every** async Rclunk is ownerless — constant background; so is a Tclunk whose owner died or abandoned it. Classified from the session table (`outstanding[tag].active && .kind == P9_TCLUNK`), so an Rlerror answering a clunk counts here, and an Rclunk on any other tag falls to the residue |
| `demux_orphan_flush` | the #845 abandon path sends its Tflush ownerless, so every abandon's Rflush lands here — death-driven. Classified from the session table (`outstanding[tag].active && .kind == P9_TFLUSH`): an Rflush on any other tag falls to the residue |
| `demux_orphan_late` | an abandoned op's late ORIGINAL reply, classified from the session table (`outstanding[tag].active && (.awaiting_flush \|\| .abandoned)`) under the same `c->lock`; a walk's bind goes to the orphan sink |
| `demux_orphan` | **the residue** — a frame no living mechanism accounts for |

The three named flows dispatch against a tag in flight
(`ownerless_dispatch_locked`), and a dispatch that fails -- a reply no parser
accepts, or of the wrong type -- left that tag and its slot held, so the
session fails closed (`client_mark_dead_locked`), as it does for an owned
reply. A reply on an `awaiting_flush` tag never fails: dispatch absorbs it
whatever its shape (`honour_late_walk` binds only a well-formed walk), and the
Rflush frees the tag. In the late arm only an `abandoned` (flush-less, #53)
op's reply can fail. The residue's dispatch is best effort and never fatal.

Only the last is a defect signal, and it reads **zero on every healthy
boot including death flows**, which is what makes it usable: a
single-digit non-zero is a misroute, tag corruption, or genuine loss
surfacing. The first four per client are logged. Collapsing any of the
three named flows back into the residue would restore the original
condition, where a constant stream of legitimate async Rclunks buried
the one frame that mattered.

The snapshot (`p9_client_ctl_snapshot`, surfaced at `/ctl/9p-sessions`)
also carries up to `P9_CTL_INFLIGHT_MAX` (8) in-flight tags — per tag the
done/async flags plus the sent T-type and primary target fid, read from
`outstanding[]` under the same lock. The design choice worth naming: a
parked op is identified by **what it waits on**, not by which thread
holds it, because the thread is the thing you cannot see from `/ctl`.
Only `p9_attached` sessions are listed (the sole production funnel); raw
test clients carry the counters unlisted.
Since 2026-10-06 a listed session's counters and tags read as numbers only to
the principals at its two ends, the system principal and a hostowner
([[sub-kernel-devctl]], [[dec-2026-10-06-9p-sessions-ends]]).

## Data structures

- `struct p9_client` (~36 KiB): embedded session (fid + 64-wide outstanding
  tables), transport vtable, the inline 32 KiB `out_buf`, `c->lock`,
  `inflight[]` (tag-indexed rpc pointers), `reader_active`,
  `send_progress` + `send_waiters` + `send_waiters_list`, `role_waiters` +
  `role_waiters_list` (threads waiting for the reader role itself, not a
  reply: the Loom ENTER), `done_reply_buf`,
  `dead`. Magic `P9_CLIENT_MAGIC` (`_Static_assert`-pinned).
- The per-session policy bits, all stamped on the still-private client
  before the root Spoor publishes and never flipped: `loose` (the B1 I-38
  opt-in) and the identity cape `cape` / `cape_uid` / `cape_gid`
  (IDENTITY-DESIGN 3.2: on a caped session every stat reports `cape_uid` as
  owner and `cape_gid` as group, keeping the server's mode).
  `p9_client_set_cape(c, uid, gid)` is the one stamp; `p9_client_init`
  resets all three (a reused client struct must not inherit a cape -- the
  kernel tests reuse one). The client only HOLDS them: the attach layer
  decides ([[sub-kernel-ninep-attach]]) and dev9p and Loom consult them
  ([[sub-kernel-ninep-dev9p]], [[sub-kernel-loom]]). The remote declaration
  `remote` (LR-1, HAUL-DESIGN 4.8) is a fourth, under the same rule:
  `p9_client_set_remote(c)` is its one stamp, made by either attach path
  before the root publishes, and `p9_client_init` resets it. It only
  NARROWS: `dev9p_spoor_remote` reads it for `territory_format_ns` and, as
  dev9p's `remote` slot, for the resolver, which contains a link the session
  serves beneath its mount (DISTRO 4.6, [[sub-kernel-stalk]]); nothing that
  checks permission, caches or vouches for exec consults it.
- `struct p9_rpc` (stack-allocated per op): tag, `done`/`dead`/`be_reader`
  flags, `sending` (registered, not yet waiting in `client_wait`: the
  handoff skips it), its OWN single-waiter rendez, `reply_buf`, `on_complete` (the
  async seam), `stop_parked` (its thread is parked for a stop inside the
  client: the handoff and the owed check skip it), and the flush(5) state of a sync op a caught note interrupted:
  `noted` (later waits killable only), `flushing` (its Tflush is on the
  wire), `honoured` + `honour_rc` + `flush_out` (a reply applied by the
  demux), `flushed` (the Rflush came first). Async containers are
  zero-allocated, so all of these read false there.
- `p9_session.outstanding[]` entry states: active · `awaiting_flush`
  (reserved until Rflush) · `abandoned` (owner gone, no flush in flight —
  freed by the late reply; excluded from `any_outstanding_on_fid`).
- Per-Thread latches (in `struct Thread`): `stop_no_park`, `stop_unwinds`,
  `stop_unwound` — owner-written only, same-call-stack read.

## Concurrency

The discipline lives in [[lock-9p-client-c-lock]]; load-bearing here:

- `c->lock` is NEVER held across the blocking recv or any sleep.
- Every park is register-then-observe: the per-rpc rendez re-checks
  `done`/`dead`; the send park registers its hook + snapshots
  `send_progress` under the lock and re-checks under its own rendez lock
  (the poll.tla shape). No lost wake — [[inv-i9]].
- `out_buf` is never re-read after a lock drop (the spill contract); the
  sole exception is the NOTAG handshake on a still-private client.
- The reader role is released across a death OR a debug/job stop at a frame
  boundary only; all FOUR `reader_active` sites (election, self-pump, the
  two pump_once variants) handle a stop-unwound recv without latching the
  session; `client_send_flow` + `client_drain_until_free_tag` park a stopped
  sender at loop-top (spilling first) so a stop can't spin or hang.
- No client waiter parks in place for a stop (DEBUG-FS 5c.6, the
  2026-09-30 waiters-and-stops amendment). Every client sleep sets
  `stop_unwinds` -- the reader recv at a frame boundary, the non-reader rpc
  sleep, the send/tag progress park -- so a stop returns `SLEEP_INTR` and the
  caller's loop parks the thread in `client_debug_stop_park`, bracketed by
  `rpc->stop_parked` under `c->lock`; on resume it re-runs the election. A
  waiter parked in place re-slept on resume without re-electing, after a
  departing reader had skipped it as stopped.
- A handoff that leaves the role free and undesignated wakes
  `role_waiters_list`, as does the session's death. Its hooks are registered
  under `c->lock` against a `reader_active` sample taken there, and every
  release of the role runs the handoff under `c->lock`: register-then-observe.
- kproc threads (SQPOLL, dev9p_poll pump) are stop-immune not via
  `t->proc == NULL` but because `proc_debug_stop_deliver` rejects kproc —
  `debug_stop_req` is always 0 there.
- The completion seam (`on_complete`) runs under `c->lock`: no sleep, no
  poll-state lock, no `p9_client_*` re-entry, atomics only.
- The death hangup runs under `c->lock` too. Its op is spinlocks and wakes
  whose locks nest after `c->lock`, and neither pipe.c nor srvconn.c calls
  the client, so nothing ranks above it ([[lock-9p-client-c-lock]]).
  `p9_client_close` closes the transport under the same lock, so a hangup
  and the close never overlap, and the hangup skips a CLOSED transport.

## Invariants enforced

![[inv-i9#Statement]]

![[inv-i10#Statement]]

![[inv-i11#Statement]]

Enforcement sites: the register-then-observe parks + `client_mark_dead_locked`'s
total wake (I-9); `alloc_tag`/`clear_outstanding` + the
`awaiting_flush`/`abandoned`/`abort_unsent` retirement discipline (I-10);
`p9_session_send_clunk`'s send-time unbind + the monotonic fid allocator
(I-11).

## Error paths

- `-EINVAL` NULL/magic mismatch · `-EBUSY` before handshake · `-EIO`
  send/recv failure, malformed frame, tag pool full, fid conflict ·
  `-<ecode>` the server's Rlerror, **its wire ecode bounded to `[1,4095]`
  HERE in `map_error` before negation** (`ecode == 0 || ecode > 4095 ->
  -EIO`) — which closes the signed-overflow UB of `-(int)0x80000000` (a
  kernel halt reachable by ANY hostile `Rlerror` on ANY op, not just attach;
  it traps under `-fsanitize=undefined`) and folds the malformed
  `Rlerror(ecode=0)`-as-success corner into `-EIO`. I-14's hostile-ecode
  bound is realised here, not deferred to dev9p (the A-3c audit F1 fix; the
  sibling wire codec [[sub-kernel-ninep-wire]] asserts the same bound).
- Congestion is NOT an error path: EAGAIN → spill/pump/park/retry; a
  stopped reader → role release, no latch; a dying owner → Tflush or the
  abandoned-bit reclaim; a caught note → Tflush and a killable wait for the
  first answer (flush(5)). Only a genuine break (or demux violation) latches
  `c->dead`, which fails everything `-EIO` including parked senders.
- Partial walks (`nwqid < nwname`) return `-EIO` at this layer (the
  resolver's pounce handles partial semantics above).

## Performance

Per op: 1 send + ≥1 recv + one `kmalloc(recv_cap)` reply buffer + one frame
copy (reader → owner). Payload clamps bound every frame to the negotiated
msize (32 KiB default; 128 KiB bulk FS sessions — the write clamp is
load-bearing, the read clamp is belt). The struct is ~36 KiB, mostly the
inline out_buf + session tables; at most one `done_reply_buf` held between
completions. A buffer pool / read-into-owner-buffer is a recorded v1.x
optimization.

## Prosecution

What an auditor attacks here (the single home of the trigger-row content for
this surface):

- **Tag/fid lifecycle** (I-10/I-11): any new retirement path must be one of
  reply / Rflush / never-sent / abandoned-late-reply — a misclassified
  partial-push reclaim breaks the stream AND I-10; `abort_unsent` must stay
  fail-soft and target only an own still-active tag; the never-sent
  classification must remain exactly the zero-bytes-pushed set (verify the
  per-transport all-or-nothing contract for any new backend).
- **The shared-session latch set**: `client_mark_dead_locked` must remain
  the sole `c->dead` setter and must remain reachable ONLY from genuine
  breaks — every congestion-class event (EAGAIN, stop, dying self) must
  dispose without latching. Prosecute every NEW send/recv error arm against
  this rule; three chunks independently got it wrong before the rule was
  named.
- **The spill contract**: no path may re-read `out_buf` after
  `client_pump_or_park_locked` (or any lock drop) has run; a spill must be
  taken BEFORE the first park; spill-OOM fails closed.
- **Frame-atomicity**: any new interrupt/unwind path out of the reader recv
  must route through the boundary latches (`stop_unwinds`/`stop_no_park`) —
  never a fresh flag, never a mid-frame unwind; classification must use the
  stable `stop_unwound` latch, never a re-read of `debug_stop_req` (an async
  resume races it); DeathWinsOverStop at every branch.
- **Role-release completeness**: all FOUR `reader_active` sites must handle
  stop/death without stranding the role or the session; the handoff must
  skip an rpc parked for a stop (`stop_parked`, set only inside
  `client_debug_stop_park`; a dying thread never parks, and
  `client_stop_pending` and the park it enters must agree, or the loops that
  park on it spin) AND rpcs still `sending`, AND re-hand-off on a DIED return
  gated on `be_reader`; a handoff that designates nobody wakes the role-waiter
  list. A new place a registered rpc's thread can sleep outside `client_wait`
  must set `sending`.
- **Park machinery**: every park on shared-reachable state uses the
  multi-waiter list ([[haz-single-waiter-rendez]]); register-then-observe
  under the documented lock order; no stale hook survives a return.
- **Reply-buffer lifetime**: any new zero-copy-aliasing op must keep the
  aliased buffer alive past the caller's copy-out (the `done_reply_buf`
  discipline).
- **The progress signal** (2026-09-30): a new path that frees a tag, or a new
  reader that departs, must call `client_send_progress_signal`, or a sender
  parked for a tag or ring space sleeps on beside a free one. A tag drainer
  must park, not pump, while a tag is owed (`client_tag_owed_locked`); a
  self-pump blocked in the transport recv sees no client-side progress
  (OPEN-BUGS 2026-09-30 11:01Z, the #349 root).
- **The flush(5) arm** (2026-09-30): a reply on a flushing owner's tag must be
  applied BEFORE any later frame (the Rflush frees the tag); the owner may
  never touch `inflight[tag]` after `honoured` or `flushed` (the tag may
  already belong to another op); every wait after `noted` must be killable
  only (a pending caught note would spin a note-interruptible one); death
  must still win in the flush wait; the Rflush-first hand-off must land in
  the dispatch's own critical section; a Tflush's send must stop at its op's
  own reply (sent after it, the Tflush would leave the stored reply to
  `dispatch_rmsg`, which absorbs a reply on an `awaiting_flush` tag: the call
  would succeed with an empty result, a read's false EOF); a living owner's
  flushed op must count live for the fid exclusion until it has acted, and the
  death-in-flush-wait arm must clear that before the Rflush.

- **Stops vs the reader role and the tag pool** (DEBUG-FS 5c.6, the
  2026-09-30 waiters-and-stops amendment): a new sleep inside the client must
  set `stop_unwinds` and return to a loop that parks via
  `client_debug_stop_park`, or a stopped waiter parks in place and re-sleeps on
  resume without re-electing. `rpc->stop_parked` is written only by the parked
  thread under `c->lock`; the handoff and `client_tag_owed_locked` read it and
  never the Proc's stop flags, and the park clears it when it returns. The park
  needs `stop_no_park` clear, as every caller leaves it: a set one makes
  `sleep()`'s death check read the park as a reader mid-frame, which a death does
  not unwind. Every `reader_active = false` site must run the handoff, whose
  no-designee exit is the Loom ENTER's only wake when a foreign sync reader
  leaves its async reply unread. Witnesses:
  `9p_client.stopped_waiter_elects_on_resume`, `.resumed_waiter_is_designated`,
  `.stop_parked_owner_not_owed`, `.note_flush_stop_parked_staging_not_owed`,
  `.handoff_skips_restopped_owner`, `.handoff_skips_stop_parked`,
  `.role_wait_contract`, `.loom_enter_wakes_when_role_frees`. Model:
  `specs/loom_role.tla`, the handoff with both stop rules and the role-waiter
  wake (`NoMissedRoleWake`). Known and tracked: a stop-parked owner holds its
  tag until resumed (the tag-pool design entry in OPEN-BUGS).

## Seams

Open: [[seam-841-mi-harness]] · [[seam-350-async-eagain]] ·
[[seam-845-untrusted-server]] · [[seam-56-netd-cancelled-tag]] ·
[[seam-90-hung-server]]. Closed, kept for the record:
[[seam-90-death-half]].

## Caveats

1. `struct p9_client` is ~36 KiB — never on a stack frame.
2. `read`/`readdir`/`readlink` are COPY semantics at the public API; the
   internal zero-copy alias is valid only under the `done_reply_buf`
   discipline.
3. Partial walks are `-EIO` at this layer.
4. Rlerror ecodes are bounded to `[1,4095]` here (`map_error`) before
   negation — NOT passed through unbounded — yielding the `[-4095,-1]`
   passthrough window; a zero or out-of-range ecode collapses to `-EIO`
   (A-3c F1, closing the `-(int)0x80000000` signed-overflow UB).
5. No retry/reconnect — a dead session stays dead until destroy + re-init.
6. Callers do NOT serialize (the old serial client's external-serialization
   contract is retired); the client serializes internally.
7. The one-reply-per-tag trust envelope ([[seam-845-untrusted-server]]).
8. An abandoned walk's late Rwalk binds a fid its dead Proc cannot clunk.
   Since 2026-09-29 the orphan sink hands it to the closer, which clunks it;
   a test client without a sink keeps it bound until the session ends
   (`orphan_kept`).

## Provenance

(generated — incoming `touched` backlinks, newest first; never hand-written.
Until the renderer emits this section, walk the backlinks of this id in
`record/changes/`: the [[lin-9p-client]] members are the curated spine.)
