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
updated: 2026-08-14
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
  `clunk` / `clunk_async` (fire-and-forget; ownerless Rclunk drain).
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
  `p9_client_abandon_async`.

Error convention: `-EINVAL` bad args/magic · `-EBUSY` not-OPEN · `-EIO`
lower-layer failure · `-<ecode>` the server's Rlerror passed through
verbatim (any u32 — callers of Stratum-extension surfaces may need to
translate STM_E* codes).

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
debug-stopped owners (`p9_rpc.owner->debug_stop_req`) so the role lands on a
runnable survivor — with `be_reader` as a pure advisory wake-hint (election
is gated solely by `reader_active` under the lock, so two readers are
impossible regardless of how many carry the hint).

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
`p9_session_abort_unsent` — zero bytes reached the wire, so I-10-safe.

**Abandon on death.** A Proc dying mid-op NULLs `inflight[tag]`, frees its
reply_buf, and sends `Tflush(oldtag)`; the tag stays reserved
(`awaiting_flush`) until its Rflush — never freed by a late original reply.
The flush sends are EAGAIN-aware WITHOUT pumping (a dying thread must not
park): on EAGAIN or a failed build, `p9_session_flush_rollback` /
`p9_session_mark_abandoned` fall back to the ownerless reclaim (the
`abandoned` bit: the late original reply frees the tag; the victim is
excluded from `any_outstanding_on_fid` so a cancel-then-close Tclunk still
sends). Only a genuine transport break latches the session.

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
header, out-of-range tag, oversize); it fails every in-flight rpc `-EIO` and
wakes both the per-rpc rendezes and the parked-sender list. A dead session
rejects all subsequent ops; there is no reconnect (destroy + re-init above).

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
| `demux_orphan_clunk` | `p9_client_clunk_async` never registers `inflight[tag]`, so **every** async Rclunk is ownerless — constant background |
| `demux_orphan_flush` | the #845 abandon path sends its Tflush ownerless, so every abandon's Rflush lands here — death-driven |
| `demux_orphan_late` | an abandoned op's late ORIGINAL reply, classified from the session table (`outstanding[tag].active && .awaiting_flush`) under the same `c->lock` |
| `demux_orphan` | **the residue** — a frame no living mechanism accounts for |

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

## Data structures

- `struct p9_client` (~36 KiB): embedded session (fid + 64-wide outstanding
  tables), transport vtable, the inline 32 KiB `out_buf`, `c->lock`,
  `inflight[]` (tag-indexed rpc pointers), `reader_active`,
  `send_progress` + `send_waiters` + `send_waiters_list`, `done_reply_buf`,
  `dead`. Magic `P9_CLIENT_MAGIC` (`_Static_assert`-pinned).
- `struct p9_rpc` (stack-allocated per op): tag, `done`/`dead`/`be_reader`
  flags, its OWN single-waiter rendez, `reply_buf`, `on_complete` (the
  async seam), `owner` (the submitting Proc — the handoff skip's key; NULL
  for async).
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
- kproc threads (SQPOLL, dev9p_poll pump) are stop-immune not via
  `t->proc == NULL` but because `proc_debug_stop_deliver` rejects kproc —
  `debug_stop_req` is always 0 there.
- The completion seam (`on_complete`) runs under `c->lock`: no sleep, no
  poll-state lock, no `p9_client_*` re-entry, atomics only.

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
  `-<ecode>` Rlerror verbatim (hostile ecodes bounded at the dev9p layer per
  I-14, not here).
- Congestion is NOT an error path: EAGAIN → spill/pump/park/retry; a
  stopped reader → role release, no latch; a dying owner → Tflush or the
  abandoned-bit reclaim. Only a genuine break (or demux violation) latches
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
  skip debug-stopped owners AND re-hand-off on a DIED return gated on
  `be_reader`.
- **Park machinery**: every park on shared-reachable state uses the
  multi-waiter list ([[haz-single-waiter-rendez]]); register-then-observe
  under the documented lock order; no stale hook survives a return.
- **Reply-buffer lifetime**: any new zero-copy-aliasing op must keep the
  aliased buffer alive past the caller's copy-out (the `done_reply_buf`
  discipline).

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
4. Rlerror ecodes pass through verbatim (u32-unbounded here).
5. No retry/reconnect — a dead session stays dead until destroy + re-init.
6. Callers do NOT serialize (the old serial client's external-serialization
   contract is retired); the client serializes internally.
7. The one-reply-per-tag trust envelope ([[seam-845-untrusted-server]]).
8. An abandoned walk leaks its server-side fid for the connection lifetime
   (a dead Proc can't clunk what its late Rwalk bound); bounded per client;
   a session-teardown fid sweep is the v1.x answer if pressure appears.

## Provenance

(generated — incoming `touched` backlinks, newest first; never hand-written.
Until the renderer emits this section, walk the backlinks of this id in
`record/changes/`: the [[lin-9p-client]] members are the curated spine.)
