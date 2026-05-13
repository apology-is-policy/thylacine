# 46. 9P transport layer

## Purpose

Frame-aware byte pipe sitting between the session state machine (`docs/reference/45-9p-session.md`) and the actual underlying transport (initially a loopback for testing; eventually a Spoor wrapping a Unix socket or in-kernel pipe to `stratumd`). The transport is responsible for **message framing** and **partial-read aggregation**; it does NOT touch tag state, fid bookkeeping, or any spec invariant. The session module's invariants compose through this layer unchanged.

Layering:

```
p9_client (caller of session + transport; future chunk)
   │
p9_session  (state machine + tag pool + fid table + dispatcher)
   │
p9_transport  (this layer: frame-aware byte pipe)
   │
p9_transport_ops  (vtable; backends provide concrete byte-pipe impls)
   │
{loopback, future Spoor-over-Unix-socket, future stratumd-backed Spoor, ...}
```

## Public API

```c
// Transport state (internal; surfaced via p9_transport_is_open).
enum p9_transport_state {
    P9_TRANS_INIT   = 0,
    P9_TRANS_OPEN   = 1,
    P9_TRANS_CLOSED = 2,
    P9_TRANS_ERROR  = 3,
};

// Backend vtable. Read(2) / write(2) semantics — partial reads allowed
// for recv; send must satisfy the full request.
struct p9_transport_ops {
    int  (*send)(void *ctx, const u8 *buf, size_t len);
    int  (*recv)(void *ctx, u8 *buf, size_t cap);
    int  (*close)(void *ctx);
    void *ctx;
};

// Lifecycle.
int  p9_transport_init(struct p9_transport *t,
                        struct p9_transport_ops ops,
                        u8 *recv_buf, size_t recv_cap);
void p9_transport_destroy(struct p9_transport *t);   // clobbers magic
int  p9_transport_close(struct p9_transport *t);     // idempotent

// I/O.
int  p9_transport_send(struct p9_transport *t, const u8 *msg, size_t len);
int  p9_transport_recv(struct p9_transport *t);      // fills t->recv_buf
int  p9_transport_round_trip(struct p9_transport *t,
                              const u8 *request, size_t request_len);

// Session composition: session.send + transport.send + transport.recv
// + session.dispatch.
int  p9_transport_exchange(struct p9_transport *t,
                            struct p9_session *s,
                            const u8 *request_msg, size_t request_len,
                            struct p9_dispatch_result *out);

// Query.
bool   p9_transport_is_open(const struct p9_transport *t);
size_t p9_transport_last_recv_len(const struct p9_transport *t);
```

### Error convention

Every public function returns `-1` on failure. Internal state transitions:

- Backend `send` returns `≤0` → transport transitions to `ERROR`; subsequent I/O refused.
- Backend `recv` returns `≤0` mid-frame → transport transitions to `ERROR`.
- Frame consistency violation (header.size != caller's `len` on send; header.size < `P9_HDR_LEN` on recv; header.size > `recv_cap` on recv) → transport transitions to `ERROR`.
- `close` is idempotent: calling it on an already-closed transport returns 0 without invoking the backend.

### Framing discipline

The transport assumes 9P2000.L's length-prefixed framing: the first 4 bytes of every message carry the total frame length (including the header itself). The send path validates `header.size == len` before calling `ops->send`; the recv path reads the 7-byte header first, peeks the size field, then reads the body in a loop until `header.size` bytes are in hand.

### Partial-read aggregation

The recv path explicitly accommodates short reads from the backend's `ops->recv` — relevant for any stream-socket backend. `do_recv` loops on `ops->recv` until either (a) it has the full frame, or (b) `ops->recv` returns `≤0` (EOF / error), at which point the transport transitions to ERROR.

The loopback backend exposes a `chunk_size` knob (default 0 = no limit) that lets tests force partial-read paths through the transport core. The `9p_transport.partial_read_aggregation` test sets `chunk_size = 3` and verifies a 28-byte Tattach round trip succeeds across multiple `ops->recv` calls.

## Compile-time invariants

`_Static_assert` in `kernel/9p_transport.c`:

- `P9_TRANSPORT_MAGIC == 0x50395452u` ("P9TR").

`_Static_assert` in `kernel/9p_transport_loopback.c`:

- `P9_LOOPBACK_MAGIC == 0x4C424B30u` ("LBK0").

## Implementation

| File | Purpose |
|---|---|
| `kernel/include/thylacine/9p_transport.h` | Transport public API + `struct p9_transport_ops` vtable + `struct p9_transport` shape |
| `kernel/9p_transport.c` | Frame validation + partial-read aggregation + session composition helper |
| `kernel/include/thylacine/9p_transport_loopback.h` | Loopback backend (test scaffold) public API |
| `kernel/9p_transport_loopback.c` | In-memory loopback impl: responder fn synthesizes responses; configurable `chunk_size` for partial-read tests |
| `kernel/test/test_9p_transport.c` | 9 unit tests covering lifecycle, round-trip, framing rejection, partial reads, backend-error propagation, session composition |

The transport struct is small (~80 bytes; magic + state + ops + recv_buf pointer + counters). The caller provides the recv buffer (typically sized to the session's negotiated `msize`).

The loopback struct holds a pointer to a caller-provided response staging buffer (not an inline buffer; the inline-buffer design was too large for the kernel's per-thread stack budget of 16 KiB — the staging buffer is now caller-allocated). The responder function receives the request bytes + a write pointer to the staging buffer, and returns the response length.

## Tests

9 tests in `kernel/test/test_9p_transport.c`:

| Test | Covers |
|---|---|
| `9p_transport.init_destroy` | Lifecycle + magic clobber + invalid-arg refusal |
| `9p_transport.round_trip` | Tclunk → echo-flipped Rclunk through loopback; tag preserved |
| `9p_transport.send_frame_size_mismatch_rejected` | Sender's `len` != `header.size` → -1 |
| `9p_transport.recv_frame_too_large_rejected` | Frame > caller's `recv_cap` → -1 + ERROR transition |
| `9p_transport.partial_read_aggregation` | 3-byte `chunk_size` forces multi-call recv loop; Tattach (28 B) succeeds |
| `9p_transport.backend_error_transitions_to_error` | Responder returns -1 → send fails → state = ERROR → further I/O refused |
| `9p_transport.close_idempotent` | Second close returns 0 (idempotent); state stays CLOSED |
| `9p_transport.exchange_drives_session_handshake` | End-to-end Tversion → Rversion → Tattach → Rattach through transport + session.dispatch |
| `9p_transport.exchange_drives_session_walk` | Full handshake + Twalk → Rwalk; new_fid bound; qid extracted |

The loopback's "session-aware" responder synthesizes correct Rversion / Rattach / Rwalk replies based on the request opcode, letting tests verify the full composition without an external server.

## Error paths

`-1` returned from:
- Lifecycle: NULL transport, magic mismatch, missing vtable function, NULL `recv_buf`, `recv_cap < 7`.
- I/O: state != OPEN, NULL message, `len < 7`, `header.size != len` on send.
- Recv: backend returned `≤0` mid-frame, `header.size < 7`, `header.size > recv_cap`.
- Compose: `p9_transport_exchange` chains all the above + the session-side `dispatch_rmsg` errors.

## Performance characteristics

- Send: 1 call to `ops->send` (backend handles loop on short writes internally; the transport defensively loops too).
- Recv: 1+ calls to `ops->recv`; the loop terminates as soon as `header.size` bytes are aggregated.
- No allocation. No locking. Single-CPU at v1.0 (per-Proc serialization is the kernel's responsibility, matching the session module's contract).

Memory: `sizeof(struct p9_transport)` ≈ 80 bytes; `sizeof(struct p9_loopback)` ≈ 80 bytes (down from ~16 KiB in the original inline-buffer design — refactored mid-chunk to caller-provided staging buffer after a stack overflow on the kernel's 16 KiB test thread stack).

## Status

| Component | State |
|---|---|
| Transport core (frame validation + partial-read aggregation) | **Landed (P5-transport)** |
| Loopback backend (test scaffold) | **Landed (P5-transport)** |
| Session composition (`p9_transport_exchange`) | **Landed (P5-transport)** |
| End-to-end session handshake through transport | **Tested (P5-transport)** |
| Spoor-over-Unix-socket backend | Phase 5+ (P5-stratumd or P5-spoor-transport) |
| Async dispatch loop (request-response decoupled) | Phase 5+ (single in-flight at v1.0; the session's tag pool will admit pipelining once the transport supports it) |
| Multi-message buffering / pipelined sends | Phase 5+ |
| Transport-layer retry / reconnect | Phase 5+ |
| Connection state machine TLA+ extension | Deferred (transport invariants are mechanical; spec extension can wait for the first async backend) |

## Known caveats / footguns

1. **The loopback's response staging buffer is caller-provided** (refactored mid-chunk from a 16 KiB inline buffer). Tests pass a `static u8 g_loopback_resp_buf[4096]` to `p9_loopback_init`. The buffer must remain alive for the lifetime of the loopback.

2. **`p9_transport_destroy` clobbers magic but does NOT call the backend's close.** Use `p9_transport_close` for graceful shutdown. The post-destroy magic clobber is the load-bearing defense against use-after-destroy (mirrors `kobj_*_unref` discipline; see `docs/reference/39-hw-handles.md` caveat #2 + the same pattern in `9p_session.c`).

3. **Once the transport transitions to ERROR, it stays there.** No automatic recovery. The caller is expected to destroy + reinit (and possibly reconnect at a higher layer). This matches the model where a torn transport requires session-level recovery — the session's tag pool / outstanding-request state cannot reliably continue past a transport failure.

4. **The send path validates only `header.size == len`** (and `len >= P9_HDR_LEN`); it does not type-check the outbound message. Type/tag/body sanity is the session module's responsibility (and the codec's parsers will catch any malformed response on the recv side).

5. **Loopback `send` refuses if the prior response hasn't been fully drained.** This catches tests that issue a second send before consuming the first response. Real backends don't have this constraint (they'd silently lose data); the loopback exposes the discipline error.

6. **`p9_transport_exchange` is request-response synchronous at v1.0.** It implements `send` then `recv` then `dispatch` in serial. Async dispatch (multiple in-flight requests dispatched on Rmsg arrival order, with `outstanding[tag]` resolving the pairing) is a Phase 5+ extension. The session module's tag pool already supports this model — only the transport-layer loop changes.

## Naming rationale

`p9_transport_*` follows the codec/session naming pattern. The `_loopback` backend name is unsurprising in test-infrastructure tradition. No marsupial-themed naming applies here — the transport is mechanical infrastructure, not a load-bearing identity surface. (The Spoor abstraction the future stream-socket backend will wrap is named per Thylacine's tradition; the transport-layer wrapping it stays neutral.)

## Spec cross-reference

No new TLA+ module at v1.0. The transport's behavioral properties (FramingIntegrity, RequestResponseOrdering, NoMessageLoss) are mechanical at the request-response synchronous level and don't require formal proof. The session module's invariants (I-10, I-11, FlowControl, OutOfOrderCorrectness in `specs/9p_client.tla`) compose through the transport unchanged.

When the first async backend lands (Phase 5+), the spec may be extended to include transport-layer connection states (CONNECTED / RECONNECTING / FAILED) + multi-message-in-flight ordering. Until then, the spec stays as it is.

## Reference

- ARCH §10 (9P client architecture).
- `docs/reference/45-9p-session.md` (session module — the layer above this one).
- `docs/reference/44-9p-wire.md` (codec — invoked by session-side dispatch).
- `specs/9p_client.tla` (session-level invariants; transport composes unchanged).
- Stratum's `stratum/v2/docs/OS-INTEGRATION.md` (the eventual Unix-socket / stratumd target).
