---
id: sub-kernel-srvconn
type: sub
title: "srvconn — the /srv per-connection byte transport"
parent: moc-kernel-srv
code: [kernel/srvconn.c, kernel/include/thylacine/srvconn.h]
audit: hard
guarded-by: [inv-i9]
validated-by: [prose, gate-smp]
locks: [lock-srvconn-chan-lock]
hazards: [haz-single-waiter-rendez, haz-death-path-wake]
abis: []
design: []
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

A `SrvConn` is one kernel-minted `/srv` connection: a bidirectional byte
transport plus the kernel-stamped identity of both ends, captured **by
value** at mint. It is pure transport + identity — since stalk-3b-β
retired the embedded per-connection 9P client, a 9P-mode connection is
driven by a separate caller-owned kernel client wrapping these rings
through the srvconn transport backend ([[sub-kernel-ninep-transport]]),
and a byte-mode connection is a POSIX-shaped stream (the pouch AF_UNIX
face). [[sub-kernel-devsrv]] is the policy layer that mints, enqueues,
and closes SrvConns; this dossier owns the rings, the flow control, the
role machinery, and teardown.

Two independent heap rings carry the bytes: `c2s` (kernel client → the
server Proc) and `s2c` (server → kernel client), each sized **2× the
connection's msize class** so a whole msize frame always fits an empty
ring with a second in flight (the #841 pipeline headroom). A frame that
cannot fit an EMPTY ring is a protocol violation (torn down); a frame
that transiently cannot fit a BUSY ring is **back-pressure, absorbed by
blocking** — the #348/#349/CF-3B arc that made all three producers and
both consumers block instead of failing.

## Contract

Declarations in `kernel/include/thylacine/srvconn.h`.

**Lifecycle** — `srvconn_create(peer_stripes, peer_pid, peer_console,
server_stripes, msize)` (born LIVE, ref 1; rejects any msize outside the
two-point class set `{SRVCONN_MSIZE, SRVCONN_BULK_MSIZE}`; NULL on OOM
with no partial state) · `srvconn_ref`/`srvconn_unref` (atomic; last
unref = teardown + magic-clobber + free the rings + the struct; extincts
on underflow/corrupt magic) · `srvconn_teardown` (LIVE→TORN, idempotent;
EOF both rings + wake everything) · `srvconn_is_live` ·
`srvconn_msize` (fail-closes to the default class so a defensive caller
never proposes past the rings).

**Identity accessors** — `srvconn_peer_stripes` / `srvconn_peer_console`
/ `srvconn_server_stripes`: immutable value copies; each revalidates
`SRV_CONN_MAGIC` and fail-closes (0/false) so a torn or freed-object
read never yields a fabricated tag.

**Mode/attach one-way setters** — `srvconn_set_byte_mode` (at mint,
before publication; flipping a kernel-attached conn extincts — a mode
flip on a published conn would corrupt a live 9P session) ·
`srvconn_set_kernel_attached` / `srvconn_is_kernel_attached`
(release/acquire pair; once set, a userspace close of the client
endpoint must NOT tear the rings down — they are load-bearing for the
kernel client; teardown migrates to the transport adapter's close).

**Deadline** — `srvconn_set_client_deadline(cn, abs_ns)` (0 = none;
clears `client_timed_out`) · `srvconn_client_timed_out` (distinguishes
-ETIMEDOUT "server hung" from -EIO "server died"). The deadline bounds
the WHOLE client recv, role wait included. Callers arm it per-op
(handshake: `SRVCONN_HANDSHAKE_DEADLINE_NS` = 5 s); the steady-state
kernel client deliberately runs deadline-0 (block until reply/EOF/death
— the #841 posture; see [[sub-kernel-ninep-transport]]).

**Transport calls** (sentinel returns, not -EXXX):

| Call | Blocking? | >0 | 0 | −1 |
|---|---|---|---|---|
| `srvconn_client_send` | no | bytes accepted | ring full | torn / bad args |
| `srvconn_client_send_frame` | no | whole frame written | no room (all-or-nothing back-pressure) | torn / bad args / frame > ring (framing bug) |
| `srvconn_client_send_blocking` | yes (c2s room) | whole n, or partial-then-EOF | — | EOF before any byte / bad args / death |
| `srvconn_client_recv` | yes (s2c data) | bytes read | EOF (torn + drained) | deadline (`client_timed_out` set) / death / bad args |
| `srvconn_server_send` | no | bytes accepted | ring full | torn / bad args |
| `srvconn_server_send_blocking` | yes (s2c room) | whole n, or partial-then-EOF | — | EOF before any byte / bad args / death |
| `srvconn_server_recv` | no | bytes read | empty-but-live (poll again) | EOF |
| `srvconn_server_recv_blocking` | yes (c2s data) | bytes read | EOF | death / bad args |

Note the deliberate EOF asymmetry: the blocking reads return **0** at
EOF (POSIX); the non-blocking server read returns **−1** at EOF and 0
for empty-but-live (the corvus poll-then-read shape).

**Poll** — `srvconn_poll(cn, events, pw)`: SERVER-endpoint semantics
(POLLIN ↔ c2s has bytes; POLLOUT ↔ s2c live with room; POLLHUP ↔
c2s.eof; POLLERR ↔ s2c.eof; both EOFs latch together). Atomic
sample+register under both chan locks; `pw == NULL` is the post-wake
sample-only call.

**Diagnostics** — `srvconn_total_created`/`_freed` (the difference is
the live count; [[sub-kernel-devsrv]]'s global soft cap reads it).

## Mechanism

**The ring** (`chan_ring_write`/`chan_ring_read`, caller holds the chan
lock): two-segment wrap-aware copies via `chan_copy` — word-wise 8-byte
strides with a byte tail through an `aligned(1) may_alias` u64 typedef
(ring offsets drift to arbitrary alignment; kernel unaligned access is
architecturally fine with `SCTLR_EL1.A == 0`, and the typedef makes it
well-defined C rather than UB — a CF-3 B pre-audit self-find). Every FS
byte crosses these rings twice, so at the 128 KiB bulk frame size the
per-byte loop was real cost.

**Produce/consume/EOF**: `chan_produce` appends what fits (−1 once EOF
latched) and wakes the consumer rendez outside the lock;
`chan_consume_nonblock` drains and wakes `wrendez` (the producer side)
on every drain — the #348 drain-wake; `chan_set_eof` latches + wakes.
The wait predicates (`chan_cond_readable` = `count > 0 || eof`,
`chan_cond_writable` = `count < cap || eof`) read the fields WITHOUT the
chan lock: `tsleep` evaluates them under the rendez lock, and every
producer mutates under the chan lock then wakes through that same rendez
lock — the happens-before that makes the lockless read sound (the
`kernel/pipe.c` discipline). `chan_cond_writable`'s `|| eof` means "stop
blocking", never "room": a woken producer re-runs `chan_produce`,
observes EOF, and fails — it never writes a full-and-torn ring.

**The roles (#354, CF-3 B)**: `reading`/`writing` are single-holder
ROLES per direction, not refusals. The holder is the only thread that
may park on that direction's `rendez`/`wrendez` — so each rendez keeps
exactly ONE possible waiter and the single-waiter convention holds
structurally ([[haz-single-waiter-rendez]] cannot fire). A CONTENDER
parks in `chan_role_acquire` on the chan's `role_waiters` list, each on
its OWN stack Rendez via a `poll_waiter` (the #349 send_waiters_list
pattern): hook registered under the chan lock BEFORE tsleep re-samples
the flag under the waiter's rendez lock — register-then-observe, no lost
wake ([[inv-i9]]); the hook is unregistered before the frame pops (no
stale hook). The acquire honors a deadline (used by the client recv) and
is death-interruptible; TIMEDOUT/INTR return WITHOUT the role.
`chan_role_release` clears the flag under the lock then wakes the whole
list. The role conds deliberately carry NO `|| eof` term (CF-3 B F2 —
[[fnd-cf3b-r1-f2]]): a contender woken by teardown while the unwinding
holder still held the role would busy-spin; liveness rests instead on
the holder's GUARANTEED release (teardown wakes the holder; every holder
exit path releases; the release wakes the contenders). Semantics:
concurrent blocking READS serialize per call; concurrent blocking WRITES
serialize per BUFFER — the role spans the whole multi-chunk delivery, so
two writers' bytes never interleave (frame/call atomicity;
`srvconn.role_park_second_writer` pins A-then-B).

**The three blocking producers** (the #348/#349 family — a POSIX
`write_full` treats a 0 return as EPIPE and closes, so a blocking write
must never return 0 on a live connection):

- `srvconn_server_send_blocking` (#348) — the server Proc's reply path
  (`devsrv_write`'s server arm). Parks on `s2c.wrendez` when full; the
  kernel client's recv drain wakes it. Closed the go-build snare:bus:
  stratumd's Rread replies filled s2c under a concurrent-fault Tread
  burst, its `write_full` saw 0 → EPIPE → closed the kernel-attached
  mount mid-build.
- `srvconn_client_send_blocking` (CF-3 B) — the byte-mode CLIENT write
  (`devsrv_write`'s CSRVCLIENT arm; the per-user stratumd proxy forwards
  whole Tmsg frames upstream with `write_full`). Parks on `c2s.wrendez`;
  the server recv paths wake it on every drain. **Every accepted chunk
  fires the conn poll list** ([[fnd-cf3b-r1-f1]] — deferring the POLLIN
  edge to end-of-delivery was a circular wait against a poll-then-read
  server: the send needs the drain, the drainer needs the edge).
- The kernel 9P client's c2s send — NOT here: `srvconn_client_send_frame`
  is deliberately non-blocking ALL-OR-NOTHING (a partial frame on the
  shared stream would desync it — [[haz-shared-stream-desync]]); its 0
  return maps to `P9_TRANSPORT_EAGAIN` and the #349 `client_send_flow`
  park/self-pump machinery lives in [[sub-kernel-ninep-client]]. The
  free-space check reads `ch->cap` (the CF-3 B wedge: a stale
  compile-time `SRVCONN_RING_CAP` bound made the first bulk frame "never
  fit" — an eternal-EAGAIN boot wedge; [[fnd-cf3b-self-freeb]]).

**The two blocking consumers**: `srvconn_client_recv` (role → loop:
drain [+ wrendez drain-wake] / EOF → 0 / tsleep on `rendez` bounded by
`client_deadline_ns`; TIMEDOUT sets `client_timed_out`) and
`srvconn_server_recv_blocking` (the c2s twin, deadline 0 — added at
P6-pouch-sockets F1: the non-blocking read's 0 was a spurious EOF to a
POSIX server racing the client's first write across CPUs).

**Teardown** (`srvconn_teardown`): flip `state` LIVE→TORN under
`cn->lock` (idempotent), release; latch BOTH `eof` flags inside ONE
dual-lock (c2s → s2c) critical section so a concurrent `srvconn_poll`
can never observe POLLHUP without POLLERR; then — outside all chan locks
— wake both consumer rendezes, both producer wrendezes, both role lists,
and the conn poll list. Residual buffered bytes still drain before EOF
surfaces to a reader.

**Free** (`srvconn_unref` last drop): teardown (idempotent) → clobber
magic → free both ring buffers → free the struct. Sound because every
blocking op's call chain holds a conn ref across its park (no thread can
be inside a ring copy at the last unref) and teardown has already
unparked every blocked party.

## Data structures

`struct SrvConn`: `magic` @0 (`SRV_CONN_MAGIC`, `_Static_assert`-pinned
— the first-u64 discriminator the KObj_Srv release path and
`devsrv_conn_of` read; cleared at free as UAF defense) · atomic `ref`
(W1.5 LSE-patchable `t_atomic_*` ops) · `lock` + `state` (LIVE/TORN,
one-way) · the by-value identity four (`peer_stripes`, `peer_pid`,
`peer_console`, `server_stripes` — no raw `Proc *`/`SrvService *`, so a
peer exit or a tombstone-rebind never turns a read into a UAF) · `msize`
(immutable class) · `client_deadline_ns` + `client_timed_out` · two
`struct srvconn_chan` (`c2s`, `s2c`) · the conn-wide `poll_list` ·
`byte_mode` (release/acquire) · `kernel_attached` (release/acquire).

`struct srvconn_chan`: `lock`, `cap`, `count`/`head`/`tail`, `eof`,
`reading`/`writing` (the roles), `rendez` (consumer) + `wrendez`
(producer — physically separate so reader and writer never share a
single-waiter slot), `role_waiters`, `buf` (heap, `cap` bytes, owned by
the SrvConn).

Constants: `SRVCONN_MSIZE` 32 KiB (default class) · `SRVCONN_BULK_MSIZE`
128 KiB (the DMSRVBULK class; = `SYS_RW_MAX`, so one max byte-I/O
syscall maps to one RPC) · ring cap = 2× class (2×64 KiB default,
2×256 KiB bulk — heap, owned by the conn, NOT #65-charged; bounded by
`SRV_MAX_CONNS` instead) · `SRVCONN_HANDSHAKE_DEADLINE_NS` 5 s ·
`SRVCONN_OP_DEADLINE_NS` 30 s (sized to corvus's worst-case
Argon2id+AEGIS+ML-KEM verb on emulated targets) · `SRVCONN_PATH_MAX` 64
· `SRVCONN_ROOT_FID` 1. Kernel-internal struct — no size assert; only
the magic offset is pinned.

## Concurrency

Locks: [[lock-srvconn-chan-lock]] (two instances; the dual-lock pair) +
the conn `state` lock. Rendez/list locks never nest inside a chan lock
(wakes after release); registers nest by design (register-then-observe).
No lock is held across any sleep.

Deadlock-freedom of the blocking mesh: a full `s2c` always has a
guaranteed drainer (the kernel client's elected reader drops `c->lock`
across every recv; when the client is itself back-pressured on c2s, the
#349 self-pump drains s2c); a full `c2s` is drained by the server's read
loop. The one un-drained composition — BOTH rings full with a peer that
never reads — is the classic full-duplex application deadlock POSIX
AF_UNIX shares; teardown / #811 death still unwinds both parked parties.

Every park is death-interruptible (#811, [[haz-death-path-wake]]):
`TSLEEP_INTR` unwinds role waits and data waits alike, returning
partial-or-−1 so the dying thread reaches its EL0-return die-check; a
partial return after INTR is safe by construction — the dying Proc
unwinds before any `write_full` retry can re-enter
([[fnd-348-r1-f2]]).

## Invariants enforced

![[inv-i9#Statement]]

On this surface: the role park's register-then-observe
(`chan_role_acquire`) · the cond/wake happens-before pairing at all five
blocking loops · the drain-wakes (`chan_consume_nonblock`,
`srvconn_client_recv`, `srvconn_server_recv_blocking` each wake
`wrendez` on every drain) · teardown's wake-everything (six wakes + the
poll list, all after the EOF latch) · the per-chunk POLLIN edge in the
blocking client send.

## Error paths

The sentinel table under Contract is exhaustive; beyond it: `srvconn_create`
returns NULL on OOM (struct or either ring — partial allocations freed)
or a non-class msize; `srvconn_ref`/`unref`/`teardown`/`set_*` extinct
on a NULL/corrupted conn (accessors and `is_live`/`timed_out`/`msize`
fail-close instead — queries degrade, mutations trap).

## Performance

Word-wise ring copies (~8× over the byte loop at bulk frame sizes); one
lock round-trip per produce/consume; wakes are no-ops when nothing is
parked. Memory: ~2×64 KiB per default conn, ~2×256 KiB per bulk conn
(+ the small struct), heap-owned; the worst-case exposure is bounded by
[[sub-kernel-devsrv]]'s `SRV_MAX_CONNS` (≈32 MiB if all 64 were bulk —
realistically only the FS mounts are). Ring sizing is the reason a
whole-frame send never blocks against an empty ring, which in turn is
why the elected reader is interruptible only at frame boundaries.

## Prosecution

What an auditor attacks here (the CLAUDE.md CF-3 B row absorbed):

- **Ring lifetime**: every blocking op must hold a conn ref across its
  park; the buffers free ONLY at the last unref, after teardown has
  unparked everyone. A path that parks without a ref, or frees with a
  copy in flight, is a UAF.
- **Role-park I-9**: register-then-observe on the role list;
  release-clears-then-wakes; a TIMEDOUT/INTR role wait must leak neither
  the role nor a stale hook (poll.tla NoStaleHook). The role conds must
  stay eof-free (re-adding `|| eof` re-opens the F2 busy-spin); the
  chan conds must KEEP their `|| eof` (removing it strands a producer at
  teardown).
- **Role/deadline composition**: the client recv's role wait honors
  `client_deadline_ns` and sets `client_timed_out` on the deadline path
  ONLY (INTR must not — the caller maps TIMEDOUT to -ETIMEDOUT).
- **Frame atomicity**: the writing role spans the whole delivery; the
  all-or-nothing `send_frame` must never partial-write (desync) and its
  free-space bound must read `ch->cap`, never a compile-time constant
  (the freeb wedge class).
- **The class policy**: `srvconn_create` rejects any msize outside the
  two-point set — an arbitrary ring_msize is an arbitrary kernel-memory
  demand.
- **Teardown atomicity**: the dual-lock EOF latch (a poller observing
  HUP-without-ERR is the bug); the wake set must stay COMPLETE (a missed
  wrendez/role/poll wake strands a parked party forever).
- **The one-way flags**: `kernel_attached`'s release/acquire pairing vs
  the userspace-close race (the close either sees the flag or the
  syscall has not yet returned the fd); `set_byte_mode` on a published
  conn must extinct.
- **Identity fail-closure**: every accessor revalidates magic — a freed
  conn must degrade to "no identity", never a stale tag.

## Seams

None open on this surface. Two adjacent items are deliberately caveats,
not debt: the client-side poll story and the future server-side
deadline (below) are unbuilt features with no v1.0 consumer, fail-closed
by construction — not owed work. The devsrv-side seams
([[seam-srv-registry-lifecycle]], [[seam-srv-9p-connect-unit]]) bound
this surface's blast radius but live there.

## Caveats

- **Client-side poll is fail-closed, not built**: `srvconn_poll` is
  SERVER-endpoint semantics; a client polling its own handle would need
  the mirror image (POLLIN ↔ s2c, POLLOUT ↔ c2s) plus its own hook list,
  so `srv_handle_poll`'s SrvConn arm returns POLLNVAL and
  `srvconn_server_send` deliberately fires NO poll wake (s2c growth is
  only a POLLIN edge for that nonexistent client poller; the kernel
  client tsleep-blocks, it does not poll).
- **A future server-side deadline needs a signal**: the TIMEDOUT
  branches in the deadline-0 blocking sends are defense-in-depth dead
  code; arming a real deadline there requires a caller-visible
  `server_timed_out` analog or `write_full` just re-parks
  ([[fnd-348-r1-f4]]).
- `client_timed_out` is sticky until the next `set_client_deadline`;
  the deadline field is read locklessly — the set-before-op discipline
  (one serialized op-driving thread) is the soundness argument.
- The non-blocking `srvconn_client_send` / `srvconn_server_send` have
  ZERO production callers post-CF-3B (devsrv routes both endpoints to
  the blocking variants; the 9P client uses `send_frame`) — they remain
  as test surface and as the poll-edge reference semantics.
- The threaded tests wait on the OBSERVABLE (`SC_YIELD_UNTIL` budgets),
  never on a single `sched()` — the bare-yield shape stopped holding
  under SMP placement (a woken peer can land on another CPU; surfaced
  as a 1-in-10 ubsan-smp8 failure of `teardown_wakes_blocked`,
  2026-07-20). A genuinely lost wake still fails: the budget expires and
  the unchanged assert fires (revert-probed by deleting the teardown
  wake). Task #77 tracks the same pattern suite-wide.

## Provenance

(generated from incoming `touched` edges — shaped by
[[chg-2026-05-19-srv-birth]] (the a3a transport + a3c identity),
[[chg-2026-05-26-16c-attach-srv]] (`kernel_attached`),
[[chg-2026-06-03-stalk3b-open-connect]] (embedded-client retirement →
pure transport), [[chg-2026-06-24-348-s2c-blocking]] (the s2c blocking
send), [[chg-2026-06-24-349-flow-control]] (the EAGAIN contract on
`send_frame`), [[chg-2026-07-08-cf3b-bulk-ring]] (heap rings + classes +
the role park + the blocking client send).)

## Tests

`kernel/test/test_srvconn.c` — 14 `srvconn.*` cases (roster verified
against `kernel/test/test.c`): `create_destroy` · `roundtrip` ·
`ring_capacity` · `recv_blocks_then_wakes` ·
`server_send_blocks_then_drain_wakes` (#348; non-vacuous — pre-fix
returned 0, no park) · `recv_deadline_timeout` · `teardown_eofs` ·
`teardown_wakes_blocked` (revert-probed against the teardown wake) ·
`bulk_ring_class` (CF-3 B; carries the freeb-wedge big-frame regression)
· `client_send_blocking_backpressure` · `client_send_blocking_poll_edge`
(the F1 regression — a poller parked on the empty ring must wake WHILE
the >cap send is in flight) · `role_park_second_writer` (A-then-B frame
atomicity) · `role_park_second_reader` · plus the srv_client byte-mode
suite exercising these rings end-to-end (listed under
[[sub-kernel-devsrv]] Tests). Threaded cases use the cooperative harness
+ the #109 terminal-park reap handshake + `SC_YIELD_UNTIL` observable
waits.
