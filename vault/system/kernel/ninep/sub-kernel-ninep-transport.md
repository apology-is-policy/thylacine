---
id: sub-kernel-ninep-transport
type: sub
title: "9P transport core + backends"
parent: moc-kernel-ninep
code: [kernel/9p_transport.c, kernel/9p_spoor_transport.c, kernel/9p_srvconn_transport.c, kernel/9p_transport_loopback.c, kernel/9p_transport_mq.c, kernel/include/thylacine/9p_transport.h]
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: [haz-shared-stream-desync]
abis: []
design: []
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

The frame-aware byte pipe between [[sub-kernel-ninep-session]] and whatever
carries the bytes. The core (`9p_transport.c`) does framing validation and
partial-read aggregation; a backend vtable (`struct p9_transport_ops`)
supplies `send`/`recv`/`close` plus two NULL-permitted deadline ops. Four
backends exist: **srvconn** (the production one — every live mount:
Stratum system FS, per-user homes, netd `/net`, corvus), **spoor** (a
Spoor-pair adapter; the SYS_ATTACH_9P pipe path), **loopback** (the
single-slot synchronous test responder), **mq** (the multi-in-flight
byte-FIFO test transport, Loom-6c — the harness the single-slot loopback
structurally could not provide).

## Contract

```
struct p9_transport_ops {
    int  (*send)(void *ctx, const u8 *buf, size_t len);
    int  (*recv)(void *ctx, u8 *buf, size_t cap);
    int  (*close)(void *ctx);
    void (*set_recv_deadline)(void *ctx, u64 deadline_ns);  // NULL-permitted
    bool (*recv_timed_out)(void *ctx);                      // NULL-permitted
    void *ctx;
};
```

- `send` contract: satisfy the full request or fail — with ONE sanctioned
  exception: `P9_TRANSPORT_EAGAIN` (**-11**) means *transient all-or-nothing
  back-pressure, zero bytes pushed* (#349). The core's `do_send` accepts
  EAGAIN only at `sent == 0`; a mid-frame EAGAIN latches ERROR (a stranded
  fragment would desync the shared stream).
- `recv` contract: read(2)-like — partial reads allowed; `0` = EOF; `-1` =
  error or deadline lapse (disambiguated by `recv_timed_out`).
- Deadline ops (Loom-4): arm an absolute-ns deadline for the NEXT recv
  (0 disarms + clears the signal). A backend leaving both NULL blocks
  unboundedly. Deadline capability is queryable
  (`p9_client_recv_is_deadline_capable`) and is a hard gate for the
  frame-boundary pumps (Loom SQPOLL, [[sub-kernel-ninep-dev9p-poll]]).
- Core API: `p9_transport_init/destroy/close` (close idempotent; destroy
  clobbers `P9_TRANSPORT_MAGIC` and does NOT call the backend close),
  `p9_transport_send` (validates `header.size == len` first),
  `p9_transport_recv` (aggregates exactly one frame into the caller-provided
  `recv_buf`), `round_trip`, `exchange` (send+recv+session-dispatch — the
  pre-#841 synchronous composition, still used by tests).

## Mechanism

**Frame aggregation** (`do_recv`): phase 1 loops `ops->recv` until the
7-byte header is in hand; peeks `size`; rejects `size < 7` and
`size > recv_cap`; phase 2 loops until `size` bytes total. Any `≤ 0` from
the backend mid-frame latches ERROR — a half-consumed frame is unrecoverable
on a shared stream ([[haz-shared-stream-desync]]). The ERROR state is
sticky: no automatic recovery; the owner destroys and re-establishes at a
higher layer.

**The srvconn backend** (production; wraps a byte-mode `struct SrvConn`'s
c2s/s2c rings):

- `send` → `srvconn_client_send_frame`: ALL-OR-NOTHING (whole frame or
  nothing — with #841 pipelining the c2s ring can transiently hold an
  undrained prior frame, and a partial write would strand a fragment).
  `0` (full-but-alive ring) maps to `P9_TRANSPORT_EAGAIN`; `< 0`
  (EOF/framing) is fatal.
- `recv` → `srvconn_client_recv`: BLOCKING; `0` = EOF (conn torn, no
  residual bytes), `-1` = deadline lapse or bad args. **Deliberately NO
  auto-arm of a per-op deadline** — the 16c R1-F2 auto-arm was REMOVED at
  #841: with the pipelined elected reader, a per-op recv timeout abandons
  one in-flight op and desyncs the byte stream shared by every Proc (the
  stalk-3c root cause). The deadline is caller-set: the handshake arms
  `SRVCONN_HANDSHAKE_DEADLINE_NS`, then [[sub-kernel-ninep-attach]] clears
  it to 0 (block until reply / EOF / death; death-interruptible per #811).
- `close`: `srvconn_teardown` + `srvconn_unref` — EOFs both rings so the
  server-side worker (stratumd's per-conn thread) wakes and exits. Runs at
  the LAST attach-session unref (with `kernel_attached` set, a userspace
  close of the KOBJ_SRV handle skips teardown — the rings are load-bearing
  for the kernel client; the adapter's close is the one legitimate
  teardown site). `p9_srvconn_transport_destroy` clobbers magic but does
  NOT unref — close-before-destroy is the discipline.
- `p9_srvconn_transport_conn(client)`: the magic-gated downcast the pts
  registry and SYS_SRV_PEER use — every transport ctx struct leads with a
  distinct u32 magic at offset 0, so a non-srvconn backend fails the check
  instead of mis-casting (both properties are `_Static_assert`-pinned in
  `9p_attach.c`: magics distinct, magic at offset 0 in both adapter types).

**The spoor backend**: routes `send` → `tx_spoor->dev->write` (looping on
short writes) and `recv` → `rx_spoor->dev->read`, offset 0 throughout
(stream semantics). `owns_spoors` decides whether close clunks the pair
(idempotent: pointers cleared before clunking; `rx != tx` guarded for the
duplex case). Leaves both deadline ops NULL — not deadline-capable, which
is exactly why the deadline-requiring pumps reject it.

**The loopback backend** (test): `send` invokes a responder function that
synthesizes the reply into a caller-provided staging buffer; `recv` drains
it in `chunk_size` pieces (forcing the partial-read paths); refuses a send
while a prior response is undrained (a test-discipline check real backends
don't have); armed-deadline + empty models a frame-boundary timeout
(`-1` + timed_out) vs disarmed EOF (`0`); `p9_loopback_force_eof` drops the
staged reply to simulate peer death.

**The mq backend** (test, Loom-6c): a spinlocked linear byte-FIFO staging N
replies concurrently — the multi-in-flight harness. Knobs the audits lean
on: `eagain_budget` (reject the next N sends with `P9_TRANSPORT_EAGAIN`,
synthesizing nothing — models #349 back-pressure where a real full ring
drops nothing), and the **#375 scribble knob** (`scribble_buf`/`len`/`arm`:
on the next recv — which runs inside the client's pump/park window where
`c->lock` is dropped — overwrite the given buffer with 0x5A, deterministically
modeling a peer rebuilding the shared `out_buf`; the
`send_backpressure_spill_survives_outbuf_reuse` regression is built on it).
The ring resets head/tail to 0 on full drain.

## Data structures

`struct p9_transport`: magic `0x50395452` "P9TR", state
(INIT/OPEN/CLOSED/ERROR), ops (by value — the ctx pointer must outlive the
transport), recv_buf/cap (caller-provided, sized to the negotiated msize by
the attach layer), last_recv_len, counters. Adapters: `p9_spoor_transport`
(magic "P9ST", tx/rx Spoor pointers, owns flag), `p9_srvconn_transport`
(magic "P9SC"-class value, one SrvConn pointer + one srvconn_ref),
`p9_loopback` (magic "LBK0"), `p9_mq_loopback` (magic "MQL0"-class,
`P9_MQ_RING_CAP` ring + scratch + knobs).

## Concurrency

The core is lock-free and single-caller by contract — the client serializes
sends under `c->lock` and gives the recv side to exactly one elected reader
at a time ([[sub-kernel-ninep-client]]). The mq backend carries its own
spinlock because the multi-in-flight tests drive it from concurrent
contexts. The srvconn backend's blocking recv inherits srvconn's
rendez/deadline machinery (that surface's own dossier documents it — the
transport only forwards).

## Invariants enforced

None directly — the transport upholds the *framing* precondition the
session's I-10/I-11 reasoning rests on (a frame boundary is never split or
merged), and the EAGAIN-at-zero-bytes contract is the premise that makes
the session's `abort_unsent` (#52) I-10-safe. Both are prose + test
validated; the spec models neither (below the abstraction —
[[spec-9p-client]]).

## Error paths

Core: `-1` + ERROR latch on backend failure, header.size mismatch (send),
short/oversize frames (recv), mid-frame EAGAIN, backend over-claiming
(`n > requested` — a backend bug check on both directions). EAGAIN
propagates ONLY from `p9_transport_send` at zero-bytes-pushed. Close is
idempotent; ops on a non-OPEN transport refuse.

## Performance

Send: one backend call per frame in the common case. Recv: ≥ 2 backend
calls (header + body) against srvconn's ring — absorbed by the ring sizes
(CF-3 B: 2× the msize class, so a whole frame fits; the reader is
interruptible only at frame boundaries because a whole frame never blocks
mid-way against a live server).

## Prosecution

- **The EAGAIN classification boundary**: EAGAIN accepted anywhere past
  `sent == 0` (core) or returned for a partial ring accept (backend) breaks
  the all-or-nothing premise → stream desync AND unsound #52 tag reclaim.
- **Deadline auto-arm regressions**: re-introducing a recv-side per-op
  deadline in the srvconn backend re-creates the stalk-3c shared-stream
  desync the #841 restore fixed (the 16c R1-F2 fix was deliberately
  REVERSED — see Caveats).
- **kernel_attached teardown asymmetry**: userspace close suppressed,
  server-side close honored, adapter close = the one teardown site; a
  change that tears down on the userspace path EOFs a load-bearing mount
  (the 16c smoking gun), one that suppresses the server path hides a dead
  peer behind an unbounded block.
- **The magic-downcast contract**: every new adapter type MUST lead with a
  distinct u32 magic at offset 0 (the `_Static_assert` pair in
  `9p_attach.c` enforces both halves for the current two).
- **Adapter/ctx lifetime**: the ops struct captures `ctx` by value-pointer;
  an adapter freed before its transport is a dangling vtable
  (`attached_destroy_inner` orders destroy-before-kfree for exactly this).

## Seams

- The 16c-F6-class multi-thread territory race and the F11/F13 hygiene
  items recorded at the 16c round live on the attach/territory surfaces,
  not here (see [[sub-kernel-ninep-attach]]).
- A hung TRUSTED server now blocks a steady-state caller indefinitely
  (death-interruptible) by design; the untrusted/remote-server story
  (deadlines, reconnect) is the same v1.x envelope as
  [[seam-845-untrusted-server]] / [[seam-90-hung-server]].

## Caveats

- **History note the code carries**: `srvconn_transport_recv` once
  auto-armed `OP_DEADLINE_NS` (16c R1-F2, refined at 16c R2-F1R2). #841
  removed it wholesale — the fix that was correct for the serial client
  became the bug under pipelining. The Record plane holds both moments;
  today's truth is caller-set deadlines only.
- ERROR is sticky; `p9_transport_exchange` is the legacy synchronous
  composition (tests only — the client's engine bypasses it).
- The loopback's staged-response + refuse-second-send discipline makes it
  structurally single-in-flight; multi-in-flight coverage REQUIRES the mq
  backend ([[seam-841-mi-harness]] tracks the still-owed cross-Proc SMP
  variant).

## Provenance

(generated from incoming `touched` edges — shaped by P5-transport,
P5-spoor-transport, 16c [[chg-2026-05-26-16c-attach-srv]], #841/#349/#375
on the srvconn arm, Loom-4 deadlines, Loom-6c mq, CF-3 B ring classes.)

## Tests

`kernel/test/test_9p_transport.c` (~10 cases: lifecycle, round-trip,
framing rejections, partial-read aggregation via chunk_size=3,
backend-error latch, idempotent close, exchange-driven handshake/walk,
`9p_transport.deadline_idle_vs_eof`) +
`kernel/test/test_9p_spoor_transport.c` (~10: routing, ownership-close
semantics, transport-core composition, the end-to-end
session+transport+spoor handshake) + the srvconn arm's coverage riding
`test_9p_srvconn_transport.c` (`kernel_attached_skips_teardown_on_handle_close`
incl. the 16c-F8 part-3 adapter-close leg) and the client suites
(`9p_client.send_backpressure_*`, `9p_client.loom_multi_inflight_*` — the
mq consumers).
