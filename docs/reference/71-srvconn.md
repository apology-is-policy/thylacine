# 71 — srvconn: the `/srv` per-connection transport

**Status**: as-built at P5-corvus-srv-impl-a3a, extended at a3c. The
`SrvConn` connection object, its bidirectional `tsleep`-bounded byte
transport, and the lifecycle/teardown path landed at a3a. As of a3b the
`devsrv` per-connection layer (reference 70) is the real consumer:
`srv_conn_open_for_proc` mints a `SrvConn` on a client connect,
`SYS_SRV_ACCEPT` hands corvus the server endpoint, and `devsrv_read` /
`devsrv_write` route to `srvconn_server_recv` / `srvconn_server_send`.
a3c added the `server_stripes` field (the poster's identity, by value)
and the three peer/server accessors that back `SYS_SRV_PEER` (the
peer-identity read; reference 70).

---

## Purpose

A `SrvConn` is one kernel-minted `/srv` connection. When a client Proc
opens `/srv/<name>`, the kernel mints a `SrvConn`: a dedicated
kernel↔server 9P transport plus the kernel-stamped peer identity. It is
the per-connection layer the `/srv` service registry (reference 70)
mediates — one `SrvConn` per client Proc, never shared. The v1.0
consumer is `corvus`, the key agent, which serves each client over that
client's own `SrvConn` (CORVUS-DESIGN.md §6.2 + §6.3).

A `SrvConn` carries two things:

- **A bidirectional byte transport** — two independent rings. `c2s`
  carries the kernel 9P client's `Tmsg` bytes to corvus; `s2c` carries
  corvus's `Rmsg` bytes back. The connection's dedicated synchronous
  kernel `p9_client` reaches the transport through a `p9_transport_ops`
  vtable; corvus reaches the other side through the connection Spoor's
  Dev read/write ops (wired at a3b).
- **The kernel-stamped peer identity** — the opening client Proc's
  `stripes`, console-attachment bit, and pid, captured *by value* at
  mint time. The service poster's (corvus's) own `stripes` is captured
  the same way (`server_stripes`, a3c) — the `SYS_SRV_PEER` poster gate
  compares it against the caller. corvus reads the peer identity back
  through `SYS_SRV_PEER` (reference 70). Because every identity field is
  a value copy, a peer that exits and is reaped never turns a `SrvConn`
  read into a use-after-free.

This layer sits below `corvus.tla`'s connection-identity model and
above the 9P client/transport (`kernel/9p_client.c`,
`kernel/9p_transport.c`) and the `Rendez`/`tsleep` wait primitive
(`kernel/rendez.c`).

---

## Public API

All declarations are in `<thylacine/srvconn.h>`.

### Lifecycle

```c
struct SrvConn *srvconn_create(u64 peer_stripes, int peer_pid,
                               bool peer_console, u64 server_stripes);
void srvconn_ref(struct SrvConn *cn);
void srvconn_unref(struct SrvConn *cn);
void srvconn_teardown(struct SrvConn *cn);
bool srvconn_is_live(const struct SrvConn *cn);
```

`srvconn_create` mints a connection from the opening client Proc's
identity — `peer_stripes` / `peer_pid` / `peer_console`, captured by
value — plus `server_stripes`, the service poster's (corvus's) `stripes`
tag, also by value (the a3c parameter; the caller is
`srv_conn_open_for_proc`, which reads it from the registry entry under
the registry lock). Born `LIVE` with refcount 1; returns `NULL` on
allocation failure with no partial state. `srvconn_ref` /
`srvconn_unref` are the refcount; the last `unref` tears down, destroys
the 9P client, and frees all storage. `srvconn_teardown` transitions
the connection to `TORN` — EOF on both rings, blocked consumer woken —
and is idempotent. `srvconn_is_live` is the not-yet-torn query.

### Kernel 9P client

```c
struct p9_client *srvconn_client(struct SrvConn *cn);
void srvconn_set_client_deadline(struct SrvConn *cn, u64 deadline_ns);
bool srvconn_client_timed_out(const struct SrvConn *cn);
```

`srvconn_client` is the connection's dedicated synchronous kernel 9P
client — kernel-owned, never in a handle table (invariant C-23).
`srvconn_set_client_deadline` sets the absolute deadline (timer_now_ns
timebase; `0` = none) for the next blocking client recv and clears the
timed-out signal. `srvconn_client_timed_out` reports whether the last
blocking recv ended on the deadline — letting the op-driving path map a
failure to `-ETIMEDOUT` (corvus hung) vs `-EIO` (corvus crashed).

### Raw byte transport

```c
long srvconn_client_send(struct SrvConn *cn, const u8 *buf, long n);
long srvconn_client_recv(struct SrvConn *cn, u8 *buf, long n);
long srvconn_server_send(struct SrvConn *cn, const u8 *buf, long n);
long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n);
```

`client_*` is the kernel 9P client's side, `server_*` is corvus's side.
`client_send` writes the `c2s` ring; `server_recv` drains it.
`server_send` writes the `s2c` ring; `client_recv` drains it.
`client_recv` is the one blocking call — see Implementation.

### Peer / server identity

```c
u64  srvconn_peer_stripes(const struct SrvConn *cn);
bool srvconn_peer_console(const struct SrvConn *cn);
u64  srvconn_server_stripes(const struct SrvConn *cn);
```

The three by-value identity reads, added at a3c to back `SYS_SRV_PEER`
(reference 70). `srvconn_peer_stripes` / `srvconn_peer_console` return
the opening client Proc's immutable `stripes` tag and console-attachment
bit as captured at mint; `srvconn_server_stripes` returns the service
poster's `stripes` tag, which `SYS_SRV_PEER`'s poster gate compares
against the caller. Each accessor revalidates `SRV_CONN_MAGIC` and
**fail-closes** — `0` / `false` — on a `NULL` or corrupted `cn`, so a
bad pointer degrades to "no identity," never a fabricated tag.

### Diagnostics

```c
u64 srvconn_total_created(void);
u64 srvconn_total_freed(void);
```

`(created - freed)` is the live `SrvConn` count.

---

## Implementation

`kernel/srvconn.c`. The chunk introduces no boot-time `init` — there is
no SLUB cache and no global state beyond two diagnostic counters; every
`SrvConn` is `kmalloc`'d (the struct is ~16 KiB — two 8 KiB rings — so
`kmalloc` routes it through `alloc_pages`).

### The two channels

Each direction is a `struct srvconn_chan` — a byte FIFO with one
blocking consumer:

```c
struct srvconn_chan {
    spin_lock_t    lock;          // protects count / head / tail / eof
    u32            count, head, tail;
    bool           eof;           // teardown latched this direction
    struct Rendez  rendez;        // the single blocking consumer waits here
    u8             buf[SRVCONN_RING_CAP];   // SRVCONN_RING_CAP == 8192
};
```

`chan_ring_write` / `chan_ring_read` are the two-segment wrap-aware ring
ops (mirroring `kernel/pipe.c`'s `ring_write` / `ring_read`), run under
the channel lock. `chan_produce` appends bytes and wakes the consumer;
`chan_consume_nonblock` drains without blocking.

### No-writer-block design

The kernel 9P client is **synchronous and single-frame-in-flight** — it
holds `p9_client.lock` across a whole send-then-receive exchange, so at
most one `Tmsg` is ever in `c2s` and one `Rmsg` in `s2c`. Each ring is
`SRVCONN_RING_CAP` (8192) — twice the negotiated msize (`SRVCONN_MSIZE`,
4096), pinned by `_Static_assert(SRVCONN_RING_CAP >= SRVCONN_MSIZE)` —
so a whole frame always fits and a write never has to block. A write
that would not fit (a protocol violation — an oversized frame) is
refused: `chan_produce` returns a short count, which the transport core
surfaces as an error. The upshot: **only one direction ever blocks** —
the kernel client draining `s2c` — so the transport is a single-
consumer wait/wake per ring, not the two-way back-pressure machine a
general pipe needs.

### The blocking client recv

`srvconn_client_recv` is the only blocking call. It drains `s2c`,
blocking when the ring is empty:

```
loop:
  lock(s2c.lock)
  if count > 0:  read, unlock, return bytes
  if eof:        unlock, return 0          (EOF — torn down, drained)
  unlock
  tsleep(s2c.rendez, cond = count>0 || eof, s2c, client_deadline_ns)
  if TSLEEP_TIMEDOUT: set client_timed_out, return -1
  (TSLEEP_AWOKEN → loop)
```

The wait is `tsleep`, not `sleep`: a corvus that **hangs** (no EOF
forthcoming) is bounded by `client_deadline_ns` — the recv returns `-1`
with `client_timed_out` set, and the op-driving path reports
`-ETIMEDOUT` rather than wedging the client forever (CORVUS-DESIGN
§6.2). A corvus that **crashes** is handled by the EOF path: teardown
latches `eof` and wakes the consumer, which returns `0` at once. The
deadline is the backstop for the hang case only.

The `cond` predicate (`chan_cond_readable`) reads `count`/`eof` without
the channel lock — `tsleep` evaluates it under `rendez->lock`, and the
producer mutates the channel under `ch->lock` then calls `wakeup()`,
whose `rendez->lock` acquisition is the happens-before. This is exactly
`devpipe`'s `cond_can_read` discipline (`kernel/pipe.c`; `specs/pipe.tla`).

The `Rendez` is single-waiter. The convention holds because the kernel
9P client serializes every op on `p9_client.lock`, so at most one
thread drains `s2c` at a time.

### Transport vtable

`srvconn_create` configures the embedded `p9_client` over a
`p9_transport_ops` whose `send` / `recv` / `close` wrap
`srvconn_client_send` / `srvconn_client_recv` / `srvconn_teardown`, with
`ctx` the `SrvConn` itself. The `p9_client` is initialized (no I/O); the
`Tversion`/`Tattach` handshake is driven later by the op-driving path
(the CORVUS-DESIGN §6.2 step-5 drive — P5-corvus-srv-impl-b, once corvus
is a real 9P server that can answer it).

### Teardown ordering

`srvconn_teardown` (CORVUS-DESIGN §6.2): under `cn->lock`, flip `state`
`LIVE → TORN` (idempotent — a second call returns at once); release
`cn->lock`; then latch `eof` on each ring and `wakeup` its consumer.
`cn->lock` is released before the channel locks are taken, so there is
no lock-ordering relation between them. After teardown every blocking
recv and every send fails fast; residual buffered bytes still drain
before EOF surfaces.

### Refcount + free

`ref` is an atomic `int` (create → 1; `ACQ_REL` on the decrement, as in
`kernel/pipe.c`'s ring refcount). The last `srvconn_unref` tears the
connection down (idempotent), `p9_client_destroy`s the client, clears
`magic` (UAF defense) **before** the `kfree`s, then frees the client,
the receive buffer, and the `SrvConn`.

---

## Data structures

### `struct SrvConn` — `<thylacine/srvconn.h>`

```c
struct SrvConn {
    u64                 magic;             // SRV_CONN_MAGIC at offset 0
    int                 ref;               // refcount; create → 1 (atomic)
    spin_lock_t         lock;              // protects `state`
    enum srvconn_state  state;
    u64                 peer_stripes;      // peer Proc's stripes (by value)
    int                 peer_pid;          // peer Proc's pid (by value)
    bool                peer_console;      // peer's console-attachment bit
    u64                 server_stripes;    // poster (corvus) stripes (by value)
    u64                 client_deadline_ns;// next blocking-recv deadline; 0 = none
    bool                client_timed_out;  // last client recv hit the deadline
    struct srvconn_chan c2s;               // kernel client → corvus
    struct srvconn_chan s2c;               // corvus → kernel client
    struct p9_client   *client;            // dedicated synchronous 9P client
    u8                 *recv_buf;          // the client's 9P receive buffer
};
```

`peer_stripes` / `peer_pid` / `peer_console` are the *client* side of
the connection's identity; `server_stripes` (a3c) is the *server*
(poster) side — the poster's `stripes` tag at mint. Carrying the poster
identity as a bare `u64` value, rather than a `struct SrvService *`
back-pointer into the registry, deliberately keeps the `SrvConn` free of
cross-object lifetime coupling: the poster can exit and its registry
slot be reused without the `SrvConn` holding a stale pointer.
`SYS_SRV_PEER`'s poster gate is the only reader.

`magic` (`SRV_CONN_MAGIC == 0x535256434F4E4E00`, `'SRVCONN'\0`) sits at
offset 0 — pinned by `_Static_assert`. It is distinct from
`SRV_SERVICE_MAGIC` (`<thylacine/devsrv.h>`) so the a3b `KObj_Srv`
handle-release path can discriminate a connection object from a service
object by the first `u64`. It is cleared on free so a read of a freed
`SrvConn` fast-fails. The struct is purely kernel-internal — never
on-disk, on-wire, or ABI-exposed — so it carries no `_Static_assert` on
total size.

### `enum srvconn_state`

| State | Value | Meaning |
|---|---|---|
| `SRVCONN_STATE_LIVE` | 1 | transport open, both directions usable |
| `SRVCONN_STATE_TORN` | 2 | torn down — both rings carry EOF; terminal |

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `SRVCONN_MSIZE` | 4096 | the connection's negotiated 9P msize |
| `SRVCONN_RING_CAP` | 8192 | per-direction ring capacity (≥ msize) |
| `SRV_CONN_MAGIC` | `0x535256434F4E4E00` | `struct SrvConn` sentinel |

---

## State machines

**Connection lifecycle** — one-way:

```
   srvconn_create            srvconn_teardown
        │                         │
        ▼                         ▼
      LIVE ───────────────────► TORN   (terminal; idempotent re-entry)
```

**Per-channel EOF latch** — `eof` is `false` at create, latched `true`
once by teardown, never cleared. A consumer drains residual bytes while
`count > 0`, then sees `eof` and reports EOF (`client_recv` → `0`,
`server_recv` → `-1`).

Neither machine has its own TLA+ module — see Spec cross-reference.

---

## Spec cross-reference

This chunk adds **no new spec module** and changes none. The transport
is plumbing — the same posture `kernel/9p_transport.h` documents ("No
new TLA+ spec at v1.0"). Its two load-bearing properties compose from
already-checked specs:

| Property | Pinned by |
|---|---|
| The `s2c` blocking drain loses no wakeup between the empty-ring check and the sleep | `specs/scheduler.tla` `NoMissedWakeup`, `specs/tsleep.tla` (the `tsleep` deadline-bounded `Rendez` wait) |
| Teardown's EOF-then-wake releases a blocked consumer | mirrors `specs/pipe.tla`'s `CloseWrite` (the EOF-propagation wake) |

`specs/corvus.tla`'s connection layer — `SrvBind` / `SrvAccept` /
`SrvPeerOp` — models connection *identity and lifecycle*, a level above
these bytes; it is implemented by the `devsrv` layer (reference 70) at
P5-corvus-srv-impl-a3b / -a3c. a3c's `server_stripes` and the three
peer/server accessors are the by-value identity store that
`SYS_SRV_PEER` reads — the `ConnOpIdentityIsKernelTruth` invariant
(the peer identity is the kernel's record, not the client's word) rests
on them. a3a / a3c introduce no spec *action* of their own, so they add
no `SPEC-TO-CODE.md` row; the `SrvPeerOp` row is owned by `SYS_SRV_PEER`
in `kernel/syscall.c` (reference 70's Spec cross-reference).

---

## Tests

`kernel/test/test_srvconn.c` — 7 tests:

- `srvconn.create_destroy` — `srvconn_create` stamps the peer identity
  by value; the connection is born `LIVE` with refcount 1; the stamped
  identity survives the peer Proc being freed (no UAF); the last
  `srvconn_unref` frees it.
- `srvconn.roundtrip` — bytes round-trip intact in both directions; a
  `server_recv` on an empty live ring reads `0`; a `client_recv` with
  data already buffered returns at once without blocking.
- `srvconn.ring_capacity` — a ring holds `SRVCONN_RING_CAP` bytes; a
  write into a full ring is refused (`0`, never blocked); the two-segment
  wrap is exercised in both directions.
- `srvconn.recv_blocks_then_wakes` — a `client_recv` on an empty ring
  blocks (`THREAD_SLEEPING`, the connection's `s2c` rendez waiter); a
  `server_send` wakes it and it reads the bytes.
- `srvconn.recv_deadline_timeout` — a `client_recv` past its deadline
  returns `-1` with `client_timed_out` set; a fresh deadline clears the
  signal.
- `srvconn.teardown_eofs` — after teardown the connection is not live,
  residual bytes still drain, then `client_recv` reads EOF (`0`),
  `server_recv` reads EOF (`-1`), and every send is refused; teardown is
  idempotent.
- `srvconn.teardown_wakes_blocked` — `srvconn_teardown` wakes a
  `client_recv` blocked on an empty ring; it returns EOF (`0`) — a
  corvus crash never wedges a client.

The two threaded tests use the kernel test harness's `thread_create` /
`ready` / `sched` cooperative-yield pattern (cf. `test_tsleep.c`).
Suite: 466/466 PASS × default + UBSan.

---

## Error paths

The transport calls do not use `-EXXX` codes — they return small
sentinels the caller interprets:

| Call | `>0` | `0` | `-1` |
|---|---|---|---|
| `srvconn_client_send` | bytes accepted | nothing accepted (ring full) | torn down / bad args |
| `srvconn_client_recv` | bytes read | EOF (torn + drained) | deadline expired (then `client_timed_out`), or bad args |
| `srvconn_server_send` | bytes accepted | nothing accepted (ring full) | torn down / bad args |
| `srvconn_server_recv` | bytes read | ring empty, connection live (poll again) | EOF (torn + drained) |

`srvconn_create` returns `NULL` on any allocation failure (the
`SrvConn`, its receive buffer, or the `p9_client`), or if `p9_client_init`
rejects its arguments — with no partial state left behind.

---

## poll (P5-poll-b)

A `SrvConn` is bidirectional and is held by both endpoints (the client
via a `KObj_Srv` handle, corvus via a `KObj_Spoor` server-endpoint Spoor
returned by `SYS_SRV_ACCEPT`). At v1.0 the server endpoint is the
caller that polls — corvus multiplexes its accept listener against
its accepted connection Spoors via `SYS_POLL`. The poll routes
through `devsrv_poll` → `srvconn_poll`.

The struct gained a connection-wide `poll_waiter_list poll_list`
(see 70-devsrv.md "poll" for the cross-module wiring). One list serves
both directions because the polled object IS the connection — every
readiness edge (c2s data arrival, s2c room available, teardown EOF on
either ring) is a single wake of every registered poller.

**Register-then-observe** under dual channel locks. `srvconn_poll`:
1. Acquires `cn->c2s.lock`, then `cn->s2c.lock` (fixed order — no other
   path takes both, so the dual-lock acquisition is safe).
2. Samples readiness:
   - `c2s.count > 0` → POLLIN (bytes for corvus to read);
   - `c2s.eof` → POLLHUP (teardown latched it);
   - `!s2c.eof && s2c.count < SRVCONN_RING_CAP` → POLLOUT (corvus can write);
   - `s2c.eof` → POLLERR (server-side writes EPIPE).
3. If `pw != NULL`, registers `pw` on `poll_list` — atomic with the
   sample under both channel locks (specs/poll.tla `Register`).
4. Releases s2c.lock, then c2s.lock.
5. Returns `revents & (events | POLL_OUTPUT_ONLY)` — POSIX-correct
   filtering (POLLIN/POLLOUT gated by `events`; POLLHUP/POLLERR always
   surfaced).

`pw == NULL` is the post-wake sample-only call (the second scan in
`sys_poll_for_proc` after `tsleep` returns).

**Producer wake sites**. Three places latch readiness and wake the
poll list (specs/poll.tla `MakeReady`):

- `srvconn_client_send` after a successful `chan_produce(&cn->c2s, …)`
  — c2s grew (POLLIN edge for corvus).
- `srvconn_server_send` after a successful `chan_produce(&cn->s2c, …)`
  — s2c grew (POLLOUT edge for any future client-side poller; corvus
  itself doesn't poll this).
- `srvconn_teardown` after `chan_set_eof` on both directions — both
  POLLHUP (off c2s.eof) and POLLERR (off s2c.eof) latch at once.

Each wake call happens AFTER the relevant channel lock(s) are
released (the same discipline as `chan_produce`'s `wakeup(&ch->rendez)`
or `chan_set_eof`'s wakeup). Lock chain: object → list → rendez,
acyclic.

**No deadlock with concurrent producers**. Although `srvconn_poll`
holds both channel locks at once, no producer ever takes both:
`chan_produce` / `chan_consume_nonblock` / `chan_set_eof` each take one
`ch->lock`. So a producer holding (say) `c2s.lock` while a poller is
already holding `c2s.lock + s2c.lock` is a normal contention case
(serialized on `c2s.lock`), not a deadlock.

---

## Status

| Item | State |
|---|---|
| `SrvConn` object + bidirectional transport | landed (P5-corvus-srv-impl-a3a) |
| `tsleep`-bounded blocking client recv | landed |
| teardown (EOF both rings + wake) + refcount | landed |
| dedicated synchronous `p9_client` per connection | landed (init only — handshake drive deferred to P5-corvus-srv-impl-b) |
| `devsrv` walk + connection mint + accept backlog + `SYS_SRV_ACCEPT` | landed (P5-corvus-srv-impl-a3b) |
| `KObj_Srv` connection consumer + `handle_release_obj` teardown | landed (a3b) |
| `server_stripes` field + `srvconn_peer_stripes` / `_peer_console` / `_server_stripes` accessors | landed (P5-corvus-srv-impl-a3c) |
| `SYS_SRV_PEER` peer-identity read (consumes the accessors) | landed (a3c; `kernel/syscall.c`, reference 70) |
| `poll_list` field + `srvconn_poll` + wake callouts at every readiness mutation | landed (P5-poll-b) |

---

## Known caveats / footguns

- **The consumer is the `devsrv` per-connection layer.** a3a landed the
  connection primitive ahead of its consumer; as of a3b the `devsrv`
  layer (reference 70) is that consumer — `srv_conn_open_for_proc` mints
  a `SrvConn` on a client connect and installs the client's `KObj_Srv`
  handle (obj = the `SrvConn`); `SYS_SRV_ACCEPT` hands corvus a
  `KObj_Spoor` server endpoint; `devsrv_read` / `devsrv_write` route to
  `srvconn_server_recv` / `srvconn_server_send`; and closing either end's
  handle does `srvconn_teardown` + `srvconn_unref`. Direct exercise of
  the primitive remains in `test_srvconn.c`. A production *client-open
  syscall* (a namespace open routed into `srv_conn_open_for_proc`) is
  still deferred to P5-corvus-srv-impl-b.
- **The no-writer-block design assumes the synchronous, single-frame-
  in-flight kernel 9P client.** It holds because `kernel/9p_client.c`
  serializes a whole exchange under `p9_client.lock`. A future
  pipelined/async 9P client (ARCH §21) that allowed multiple frames in
  flight would need a ring resized to the in-flight bound, or back-
  pressure on the writer.
- **The single-waiter `Rendez` on each ring** likewise relies on the
  `p9_client.lock` serialization — at most one thread drains a given
  ring. A second concurrent blocking consumer would extinct on the
  `Rendez` single-waiter assertion.
- **`client_timed_out` is sticky** until the next
  `srvconn_set_client_deadline` clears it. The op-driving path (the
  §6.2 step-5 handshake/op drive — P5-corvus-srv-impl-b) sets the
  deadline immediately before each blocking op, so each op reads a fresh
  signal; a timeout is in practice terminal for the connection (the
  op-driving path tears it down).
- **The deadline must be set before the locked 9P op.** `client_recv`
  reads `client_deadline_ns` with no lock; it is safe because the
  op-driving path sets the deadline before entering the `p9_client`'s
  locked exchange, and only that one serialized thread reads it.
- **A `SrvConn` is ~16 KiB** (two 8 KiB rings) plus a ~12 KiB
  `p9_client` and a 4 KiB receive buffer — ~32 KiB per connection. The
  global connection cap (a3b; ~256, corvus's `MAX_USERS` order) bounds
  the worst-case kernel-memory footprint.
