# 50. 9P Spoor-pair transport adapter (P5-spoor-transport)

The plumbing layer that wraps a `struct Spoor` pair into the `p9_transport_ops` vtable. Realizes ARCH §9.6's "every filesystem entity is a Spoor" principle from the transport side: a 9P client's byte pipe is whatever Spoor read/write the caller chooses (a future kernel pipe, a Unix-socket Spoor, a virtio-vsock Spoor, …). This chunk introduces no new attack surfaces — it is a thin adapter that delegates to the underlying Dev's read / write slots.

---

## Purpose

The 9P transport core (`kernel/9p_transport.c`, P5-transport) is backend-agnostic. Backends provide a vtable:

```c
struct p9_transport_ops {
    int (*send)(void *ctx, const u8 *buf, size_t len);
    int (*recv)(void *ctx, u8 *buf, size_t cap);
    int (*close)(void *ctx);
    void *ctx;
};
```

Until this chunk, the only backend was the in-memory loopback (`kernel/9p_transport_loopback.c`) — a synchronous responder fn called by `send`. This chunk adds the **Spoor-pair** backend: a pair of Spoors carries the bytes; the adapter routes `send` → `tx_spoor->dev->write` and `recv` → `rx_spoor->dev->read`.

The Spoor pair may be:
- **Two distinct Spoors** (Plan 9 `pipe(fd[2])`-style — separate read / write endpoints).
- **The same Spoor** for both directions (duplex socket — Unix socket, vsock, virtio-vsock).

The adapter doesn't care; it just dispatches through the Dev vtable.

---

## Public API — `<thylacine/9p_spoor_transport.h>`

```c
#define P9_SPOOR_TRANSPORT_MAGIC  0x50395354u   // "P9ST"

struct p9_spoor_transport {
    u32           magic;
    struct Spoor *tx_spoor;
    struct Spoor *rx_spoor;
    bool          owns_spoors;
};

int  p9_spoor_transport_init(struct p9_spoor_transport *st,
                             struct Spoor *tx, struct Spoor *rx,
                             bool owns_spoors);
void p9_spoor_transport_destroy(struct p9_spoor_transport *st);
struct p9_transport_ops
     p9_spoor_transport_ops(struct p9_spoor_transport *st);
bool p9_spoor_transport_is_open(const struct p9_spoor_transport *st);
```

### Lifecycle ownership

`owns_spoors` controls whether `transport_close` clunks the Spoors:

- **`owns_spoors = false`** — caller retains spoor_clunk responsibility. Use when the Spoors outlive the adapter (e.g., they are mounted in a Territory, or are pinned by some other holder).
- **`owns_spoors = true`** — the adapter clunks both Spoors on `close`. Use for adapter-internal Spoor pairs (e.g., a kernel-internal pipe created just for one 9P session).

The two paths satisfy the spec's `MountRefcountConsistency`-style discipline at a different layer: every Spoor reference is held by exactly one owner; the adapter explicitly chooses to be (or not be) that owner.

---

## Implementation

`kernel/9p_spoor_transport.c` (~135 LOC).

### `spoor_transport_send`

```c
static int spoor_transport_send(void *ctx, const u8 *buf, size_t len) {
    /* validate; then loop on short writes */
    size_t total = 0;
    while (total < len) {
        long n = tx_spoor->dev->write(tx_spoor, buf + total,
                                      (long)(len - total), /*off=*/0);
        if (n < 0) return -1;
        if (n == 0) return (int)total;  /* pipe closed; partial */
        total += (size_t)n;
    }
    return (int)total;
}
```

- Loops on short writes so backends that accept partial writes still produce the contract's "bytes written must equal `len` on success."
- Offset is 0 throughout: 9P framing is stream-style; the byte pipe doesn't have a meaningful seek position.

### `spoor_transport_recv`

```c
static int spoor_transport_recv(void *ctx, u8 *buf, size_t cap) {
    long n = rx_spoor->dev->read(rx_spoor, buf, (long)cap, /*off=*/0);
    if (n < 0) return -1;
    return (int)n;   /* 0 means EOF; transport core surfaces error */
}
```

- Single Dev read per call. The transport core's frame-aggregation loop calls `recv` repeatedly until a complete frame is in hand.

### `spoor_transport_close`

```c
static int spoor_transport_close(void *ctx) {
    if (st->owns_spoors) {
        struct Spoor *tx = st->tx_spoor, *rx = st->rx_spoor;
        st->tx_spoor = NULL; st->rx_spoor = NULL;
        if (tx) spoor_clunk(tx);
        if (rx && rx != tx) spoor_clunk(rx);
    }
    return 0;
}
```

- Idempotent under owns=true: the pointers are cleared before clunking, so a second close is a no-op.
- Under owns=false: pure no-op (caller cleans up).

---

## Spec cross-reference

No new TLA+ module. The adapter is pure plumbing over already-spec'd layers:

- `specs/9p_client.tla` (P5-spec) — covers session correctness (I-10 tag uniqueness, I-11 fid stability, OutOfOrderCorrectness, FlowControl). Composes unchanged through the transport layer.
- `specs/territory.tla` (P5-attach-mount) — covers mount-refcount consistency. The adapter's `owns_spoors` discipline is the caller-side counterpart: each Spoor ref has exactly one owner at any time.

No spec extension needed — the adapter introduces no new invariants. Its only correctness obligation is "delegate faithfully to the Dev vtable," which is structural in the impl.

---

## Tests

9 tests in `kernel/test/test_9p_spoor_transport.c`:

| Test | Covers |
|---|---|
| `spoor_transport.init_destroy` | Lifecycle (init sets magic + spoors + owns flag; destroy clobbers; is_open reflects state). |
| `spoor_transport.init_null_rejected` | NULL adapter / tx / rx → init returns -1; state not mutated. |
| `spoor_transport.send_routes_to_tx_dev_write` | Adapter.send writes bytes through tx Spoor's `dev->write`; rx untouched. |
| `spoor_transport.recv_routes_to_rx_dev_read` | Bytes pre-loaded into rx's backing buffer come back through adapter.recv. |
| `spoor_transport.recv_empty_returns_zero` | Empty rx → recv returns 0 (EOF; transport core surfaces error). |
| `spoor_transport.close_clunks_when_owned` | owns=true → close clunks both Spoors; idempotent on second close. |
| `spoor_transport.close_preserves_when_unowned` | owns=false → close is a no-op; Spoors stay alive. |
| `spoor_transport.transport_core_round_trip` | Wrap adapter in `struct p9_transport`; send a raw Tversion frame, synthesize Rversion in rx, transport.recv aggregates correctly. |
| `spoor_transport.end_to_end_handshake` | Full `p9_session` + `p9_transport` + spoor-adapter composition: Tversion → Rversion → INIT→VERSIONED → Tattach → Rattach → VERSIONED→OPEN; root fid bound. |

### Test fixture: byte-pipe Dev

The tests use a static `test_pipe_dev` Dev whose `read` / `write` slots back per-Spoor 8 KiB FIFO buffers. The Dev is NOT registered in the bestiary (so `dev.vtable_slot_coverage` doesn't see it); other slots are NULL (safe because `spoor_clunk`'s `dev->close` call is guarded). The fixture is reused across all 9 tests with per-test `struct test_pipe` instances in `static` storage (Spoor-test-thread stack is 16 KiB; the 16 KiB byte-pipe pair would exhaust it on the stack — the same lesson learned in P5-transport).

### End-to-end handshake test detail

The e2e test runs through the full 9P composition:
1. `p9_session_send_version(s, buf, cap, "9P2000.L", 8)` builds the Tversion frame in `buf`.
2. `p9_transport_send(t, buf, len)` routes the bytes through the adapter → `tx_spoor->dev->write` → tx buffer.
3. Test reads tx buffer to verify framing; synthesizes the canonical Rversion bytes into rx buffer.
4. `p9_transport_recv(t)` → adapter `recv` → rx_spoor's `dev->read` → recv_buf has the full frame.
5. `p9_session_dispatch_rmsg(s, recv_buf, len, &r)` parses Rversion + transitions INIT → VERSIONED.
6. Repeat with Tattach / Rattach: VERSIONED → OPEN; root fid bound.

This proves the entire stack composes end-to-end against real Spoor I/O — until this chunk, every 9P test ran through the loopback-fn shortcut.

---

## Error paths

- `init` returns -1 on NULL args.
- `send` returns -1 on adapter-magic mismatch / NULL tx_spoor / NULL `dev->write`; -1 on Dev write error; partial-write count on EOF mid-write.
- `recv` returns -1 on adapter-magic mismatch / NULL rx_spoor / NULL `dev->read`; -1 on Dev read error; 0 on empty buffer (EOF).
- `close` returns -1 on adapter-magic mismatch; 0 otherwise (idempotent).

The transport core treats -1 as "transition to ERROR sink"; 0 from `recv` as "EOF, fail the frame."

---

## Performance characteristics

- One Dev read per `recv` (transport core loops on partial reads).
- One Dev write per `send` partial-write loop iteration.
- Steady-state cost: identical to the underlying Dev — the adapter adds only `if (magic && tx_spoor && tx_spoor->dev->write)` checks per call.

---

## Status

| Component | State |
|---|---|
| Adapter `.h` + `.c` (~135 LOC) | **Landed (P5-spoor-transport)** |
| 9 unit tests | **Landed (P5-spoor-transport)** |
| Spoor-over-byte-pipe end-to-end composition | **Landed (P5-spoor-transport)** — via test scaffold |
| Real backing Spoor (kernel pipe) | Deferred — needs P5-pipe primitive |
| Real backing Spoor (Unix socket) | Deferred — Phase 5+ socket primitives |
| Real backing Spoor (virtio-vsock) | Deferred — Phase 5+ vsock primitive |

---

## Known caveats / footguns

### Adapter MUST outlive the transport it backs

`p9_spoor_transport_ops` returns a vtable struct that captures `&st` as `ctx`. If the adapter goes out of scope before the transport that uses these ops, the transport will dereference a dangling pointer on the next send / recv / close. Standard C aliasing rule; documented because the function returns the vtable by value (looks like a temporary).

### `owns_spoors=true` and dev9p Spoors

If `owns_spoors=true` and the tx / rx Spoors are dev9p-backed (rare; a 9P client wrapping another 9P client — possible in principle but unusual), the adapter's close path will call `spoor_clunk` which routes through `dev9p_close` → `p9_client_clunk` on the underlying 9P session. That's an additional 9P round trip during transport close. Document is here so callers aren't surprised.

### Offset is hard-coded to 0

The Dev vtable's read / write slots take a `s64 off` parameter for seekable devices (e.g., a future disk-backed Dev). The adapter passes 0 unconditionally; stream-style backends ignore it, but a seekable backend would read / write from byte 0 every call, which is wrong. For seekable backends, wrap them in a stream-style facade before passing to the adapter.

### Single-buffer for tests; in production, ring buffers are expected

The test fixture uses a simple "linear buffer with read / write cursors" pipe. Production byte-pipe Devs (when they land) will use ring buffers with wraparound. The adapter doesn't care — Dev read / write semantics are identical from its perspective.

---

## Naming rationale

`p9_spoor_transport` is direct ("a 9P transport backed by Spoors"). The struct name parallels `p9_loopback` and `p9_transport` in the 9P stack. No thematic marsupial alternative reads more clearly than the literal name.

---

## Reference

- ARCH §9.6 (filesystem-as-Spoor; the architectural principle this realizes from the transport side).
- `docs/reference/46-9p-transport.md` (transport core that this adapter plugs into).
- `docs/reference/47-9p-client.md` (high-level client that uses the transport).
- `docs/reference/48-dev9p.md` (the *other* direction — Dev vtable on top of 9P client; this chunk is the inverse).
- `docs/reference/30-dev-spoor.md` (the Spoor / Dev abstraction).
- ROADMAP §7.2 (Phase 5 deliverables).
