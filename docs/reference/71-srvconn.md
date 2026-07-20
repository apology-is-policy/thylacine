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
long srvconn_server_send_blocking(struct SrvConn *cn, const u8 *buf, long n);  // #348
long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n);
```

`client_*` is the kernel 9P client's side, `server_*` is corvus's /
stratumd's side. `client_send` writes the `c2s` ring; `server_recv`
drains it. `server_send` writes the `s2c` ring; `client_recv` drains it.
There are **two** blocking calls (see Implementation): `client_recv`
(blocks on an empty `s2c`) and `server_send_blocking` (#348 — blocks on
a full `s2c`). The non-blocking `server_send` remains for the corvus
poll-then-write pattern; `devsrv_write`'s server arm uses the blocking
variant so a 9P-server Proc never sees a 0-return it would misread as
EPIPE.

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
`SrvConn` is `kmalloc`'d. Since CF-3 B the ring storage is HEAP, owned
by the conn: the struct itself is small, and `srvconn_create` allocates
each direction's buffer at `2x` the conn's msize class — 2 x 64 KiB for
the default class, 2 x 256 KiB for a `DMSRVBULK` service's conns (which
also retired the old inline-array shape that rounded every conn up to a
256 KiB `kmalloc`). Exactly two classes exist (`SRVCONN_MSIZE` /
`SRVCONN_BULK_MSIZE`); `srvconn_create` rejects anything else, so ring
memory is a two-point policy, never an arbitrary demand.

### The two channels

Each direction is a `struct srvconn_chan` — a byte FIFO with one
blocking consumer:

```c
struct srvconn_chan {
    spin_lock_t    lock;          // protects count / head / tail / eof + roles
    u32            cap;           // ring capacity: 2x the conn's msize (CF-3 B)
    u32            count, head, tail;
    bool           eof;           // teardown latched this direction
    bool           reading;       // the single blocking-CONSUMER role
    bool           writing;       // the single blocking-PRODUCER role (#348)
    struct Rendez  rendez;        // the role-holding consumer waits here
    struct Rendez  wrendez;       // the role-holding producer waits here (#348)
    struct poll_waiter_list role_waiters;  // #354: parked role CONTENDERS
    u8            *buf;           // heap ring storage, cap bytes (CF-3 B)
};
```

The byte copies (`chan_copy`) are word-wise — 8 bytes at a time with
byte head/tail — since every FS byte crosses the rings twice and a
128 KiB bulk frame makes a per-byte loop real cost (kernel unaligned
u64 access is legal, SCTLR_EL1.A == 0).

`chan_ring_write` / `chan_ring_read` are the two-segment wrap-aware ring
ops (mirroring `kernel/pipe.c`'s `ring_write` / `ring_read`), run under
the channel lock. `chan_produce` appends bytes and wakes the consumer;
`chan_consume_nonblock` drains without blocking.

### Writer back-pressure (#348 + #349)

The original design assumed the kernel 9P client was **synchronous and
single-frame-in-flight** — one `Tmsg` in `c2s`, one `Rmsg` in `s2c` — so
that a single frame always fit a `SRVCONN_RING_CAP` (65536, twice the
`SRVCONN_MSIZE` 32 KiB msize, pinned by
`_Static_assert(SRVCONN_RING_CAP >= SRVCONN_MSIZE)`) ring and a write
**never had to block**. The **#841 pipeline restoration** broke that
premise: the elected-reader client now has **several frames in flight**,
so a burst of pipelined `Tmsg`s can fill `c2s` and a burst of `Rmsg`
replies can fill `s2c` faster than the far side drains them. A
non-blocking write into a full ring then returns a **short count / 0**,
which the writer mis-reads as a fatal error and **closes the
connection** — killing a live, kernel-attached mount mid-workload.

So **both directions now have real back-pressure**, each a single-
producer / single-consumer blocking pair:

| Ring | Writer (blocks on full) | Drainer (wakes the writer) |
|---|---|---|
| `c2s` | the kernel client — **#349** `client_send_flow` (block-and-self-pump at the `p9_client` layer) | stratumd / corvus `server_recv` |
| `s2c` | a 9P-server Proc via `devsrv_write` — **#348** `server_send_blocking` (parks on `s2c.wrendez`) | the kernel client `client_recv` |

The two fixes are twins. #349's was the smoking-gun that killed the
`go build` text page-in (`c2s` Twrite back-pressure); #348's is the
symmetric `s2c` case (stratumd's `Rread` replies filling `s2c` under the
compile's concurrent-fault Tread burst — a write of `0` became `EPIPE`
became a mount close became a `-P9_E_IO` text page-in became `snare:bus`).

An oversized single frame (a protocol violation, `n > SRVCONN_RING_CAP`)
is still refused by the `c2s` `client_send_frame` all-or-nothing path;
the `s2c` blocking write tolerates any `n` by multi-cycling across drain
rounds.

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

### The blocking server send (#348)

`srvconn_server_send_blocking` is the `s2c`-write twin of the blocking
client recv. `devsrv_write`'s server arm uses it so a 9P-server Proc
(stratumd) writing reply frames **delivers the whole buffer**, parking
when `s2c` is full instead of returning a `0` its `write_full` would
treat as `EPIPE`:

```
claim the single-writer role (writing busy-guard, mirror of reading)
loop:
  put = chan_produce(s2c, buf+done, n-done)   // eof-checked; wakes the reader rendez
  if put < 0:    eof -> return (done>0 ? done : -1)
  done += put
  if done >= n:  return done                  // whole buffer delivered
  tsleep(s2c.wrendez, cond = count<CAP || eof, deadline=0)   // ring full -> park
  if TSLEEP_INTR: return (done>0 ? done : -1)  // #811 death-interrupt
  (TSLEEP_AWOKEN -> loop, chan_produce again)
release the writing guard
```

Two design points make this sound:

- **A separate `wrendez`, not the reader's `rendez`.** The reader parks
  on `rendez` (waits `s2c` non-empty); the writer parks on `wrendez`
  (waits `s2c` has-room). Giving each its own `Rendez` means each has
  **exactly one possible waiter** — the single-waiter convention holds
  trivially, with no "EMPTY xor FULL never-both-parked" argument to
  prove (the #349-class single-waiter-overflow extinction is structurally
  unreachable). The drain-wake is added to `client_recv`: after
  `chan_ring_read` it `wakeup(s2c.wrendez)`, the register-then-observe
  partner of `chan_cond_writable` (I-9, the `chan_cond_readable`
  discipline mirrored).
- **The `writing` busy-guard** (set/cleared under `ch->lock`, mirror of
  `reading`) refuses a second concurrent blocking producer (`-1`,
  fail-closed) — stratumd is thread-per-connection so there is one
  writer per `s2c`, but a multi-thread Proc sharing the fd cannot trip
  the single-waiter `wrendez`. The guard is held across the whole
  multi-cycle write, so the `n` bytes enter `s2c` contiguously versus any
  other producer (frame-atomicity); the kernel `s2c` reader already
  reassembles partial frames by size (it must — the non-blocking
  `server_send` already short-writes).

Deadlock-free composed with #349: a full `s2c` always has a draining
party (the kernel elected reader, or #349's self-pump when the client is
itself back-pressured on `c2s`), and a full `c2s` likewise. No spinlock
is held across either `tsleep`; the `writing` / `reading` flags are
plain booleans the opposite role never waits on.

### The blocking-role park (#354, CF-3 B)

`reading` / `writing` are ROLES, not refusals. The role holder is the
only thread that may park on the direction's `rendez` / `wrendez` (each
stays single-waiter — the audited #348/#349 machinery is untouched). A
CONTENDING blocking party parks on the chan's `role_waiters` list —
each on its own stack `Rendez` via a `poll_waiter`, the #349
`send_waiters_list` pattern — inside `chan_role_acquire`, and
`chan_role_release` clears the flag under `ch->lock` then wakes the
list; teardown wakes it too, so parked contenders unwind on EOF. The
acquire honors a deadline (used by `srvconn_client_recv`, whose whole
recv — role wait included — is bounded by `client_deadline_ns`) and is
#811 death-interruptible.

Pre-#354 a contender was refused `-1`, which a POSIX server's
`write_full` treats as EPIPE → it closes the kernel-attached mount (the
#348-audit F1 latent). While stratumd was one-thread-per-conn that was
unreachable; CF-2's threaded request processing made the kernel's
soundness rest on stratumd's own `write_mu` — a cross-project
pre-condition the role-park retires. Semantics: concurrent blocking
READS serialize per call (each recv consumes one lock-atomic chunk);
concurrent blocking WRITES serialize per buffer (the role spans the
whole multi-chunk delivery, so two writers' bytes never interleave —
`srvconn.role_park_second_writer` pins the A-then-B order).

Two audit findings landed on this machinery and are closed in-chunk:
**F1 [P1]** — the blocking client send originally woke the conn's
`poll_list` once at end-of-delivery; a poll-then-read byte server
parked on POLLIN plus a client write larger than the ring was a
circular wait (the send needs the drain, the drainer needs the edge).
Every accepted chunk now fires `poll_waiter_list_wake` (the
per-write discipline of the non-blocking twin);
`srvconn.client_send_blocking_poll_edge` pins it. **F2 [P3]** — the
role-wait conds carried an `|| eof` term, so a contender woken by
teardown while the unwinding holder still held the role busy-spun
until the holder got scheduled; the conds now wait purely on
role-free, with liveness resting on the holder's guaranteed release
(teardown wakes the holder; every exit path releases; the release
wakes the contenders).

The family's THIRD producer also went blocking in the same pass:
`srvconn_client_send_blocking` (the byte-mode CLIENT write — the
per-user stratumd proxy forwards whole Tmsg frames upstream through its
conn Spoor with `write_full`, which treats a 0 from a transiently-full
c2s as EPIPE). It parks on `c2s.wrendez`; the server-side recv paths
(`srvconn_server_recv` / `_blocking`) now wake `wrendez` on every
drain, mirroring #348's `client_recv` drain-wake. `devsrv_write`'s
CSRVCLIENT arm routes to it (kernel-attached conns still refused).
Both-directions-full with a peer that never reads is the classic
full-duplex application deadlock POSIX AF_UNIX shares; teardown / #811
death unwinds both parked parties.

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
| `SRVCONN_MSIZE` | 32768 | the connection's negotiated 9P msize (Weft-0; was 4096) |
| `SRVCONN_RING_CAP` | 65536 | per-direction ring capacity (2x msize) |
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

`kernel/test/test_srvconn.c` — 8 tests:

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
- `srvconn.server_send_blocks_then_drain_wakes` (#348) — the `s2c`-write
  twin: a producer kthread calling `server_send_blocking` into a full
  `s2c` ring parks on the `s2c.wrendez` (`THREAD_SLEEPING`, `writing` held,
  the reader `rendez` waiter `NULL`); the main thread's `client_recv`
  drain wakes it; it delivers the whole payload (returns `n`), and the
  payload lands intact at the stream tail. Non-vacuous: pre-fix
  `server_send` returned `0` on a full ring (no park).
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

The four threaded tests (`recv_blocks_then_wakes`,
`server_send_blocks_then_drain_wakes`, `client_send_blocking_backpressure`,
`teardown_wakes_blocked`) use the kernel test harness's `thread_create` /
`ready` / `sched` cooperative-yield pattern (cf. `test_tsleep.c`) plus the
#109 terminal-park reap handshake. Default + UBSan green.

**They wait on the OBSERVABLE, never on a single `sched()`** (`SC_YIELD_UNTIL`).
The bare `sched(); assert(counter advanced)` shape assumed one yield runs the
peer to its next block — an assumption that predates SMP placement and stopped
holding once `select_target_cpu` can put the woken peer on another CPU. It
surfaced 2026-07-20 as a 1-in-10 `ubsan-smp8` failure of
`teardown_wakes_blocked`, where the harness runnable-dump showed the consumer
already woken and RUNNABLE on `cpu=4`: the wake had been delivered, the assert
just ran before it was observed. Both waits of each test are covered — the
"peer ran and is now SLEEPING" wait carries the same hazard as the wake wait.

The budget keeps the tests honest: a genuinely lost wake never satisfies the
condition, the budget expires, and the unchanged assert fails exactly as
before (revert-probed by deleting `wakeup(&cn->s2c.rendez)` from
`srvconn_teardown` — the test still fails loudly). Task #77 tracks the same
pattern elsewhere in the suite.

---

## Error paths

The transport calls do not use `-EXXX` codes — they return small
sentinels the caller interprets:

| Call | `>0` | `0` | `-1` |
|---|---|---|---|
| `srvconn_client_send` | bytes accepted | nothing accepted (ring full) | torn down / bad args |
| `srvconn_client_recv` | bytes read | EOF (torn + drained) | deadline expired (then `client_timed_out`), or bad args |
| `srvconn_server_send` | bytes accepted | nothing accepted (ring full) | torn down / bad args |
| `srvconn_server_send_blocking` (#348) | the whole `n` delivered (blocking on a full ring), or a partial then eof | — (never 0 on a live conn) | eof before any byte / bad args / a 2nd concurrent blocking producer (the `writing` guard) / #811 death-interrupt |
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
| `client_fid` + `client_handshake_done` + `client_offset` fields | landed (P5-corvus-srv-impl-b2) |
| `srvconn_drive_client_handshake` — Tversion + Tattach + (optional) Twalk + Tlopen | landed (b2) |
| `srvconn_client_read` / `srvconn_client_write` — Tread / Twrite on `client_fid`, gated on `handshake_done` | landed (b2) |
| `SYS_SRV_CONNECT(name, path)` composing `srv_conn_open_for_proc` + the handshake drive | landed (b2; `kernel/syscall.c`, reference 28) |
| per-Proc one-connection cap (`Proc.srv_conn_count`; SRV_CONN_PER_PROC_MAX = 1) | landed (b2; enforced in `srv_conn_open_for_proc`, decremented in `handle_close`) |
| KOBJ_SRV r/w dispatch in `sys_read_for_proc` / `sys_write_for_proc` | landed (b2; SRV_CONN_MAGIC routes through `srvconn_client_read/write`) |

---

## Known caveats / footguns

- **The consumer is the `devsrv` per-connection layer + the `SYS_SRV_CONNECT`
  client-open path.** a3a landed the connection primitive ahead of its
  consumer; as of a3b the `devsrv` layer (reference 70) is that consumer
  on the server side — `srv_conn_open_for_proc` mints a `SrvConn` on a
  client connect and installs the client's `KObj_Srv` handle (obj = the
  `SrvConn`); `SYS_SRV_ACCEPT` hands corvus a `KObj_Spoor` server
  endpoint; `devsrv_read` / `devsrv_write` route to
  `srvconn_server_recv` / `srvconn_server_send`; closing either end's
  handle does `srvconn_teardown` + `srvconn_unref`. b2 then added the
  production *client-open syscall* (`SYS_SRV_CONNECT` — composes
  `srv_conn_open_for_proc` with `srvconn_drive_client_handshake`); the
  KObj_Srv handle that returns to the client has its `client_fid` open,
  and SYS_READ / SYS_WRITE on it route through `srvconn_client_read` /
  `_write` (a Tread / Twrite on the SrvConn's kernel-owned p9_client).
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
