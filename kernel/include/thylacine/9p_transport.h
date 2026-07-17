// 9P2000.L transport layer (P5-transport).
//
// Sits below the session state machine (`kernel/9p_session.{c,h}`) and
// above the actual byte pipe. The session produces framed Tmsg bytes;
// the transport delivers them to a server (and conversely, the
// transport receives Rmsg bytes and the session dispatches them).
//
// Layering:
//
//   p9_client (caller of session + transport; not in this chunk)
//      │
//   p9_session  (state machine + tag pool + fid table + dispatcher)
//      │
//   p9_transport  (this layer: frame-aware byte pipe)
//      │
//   p9_transport_ops  (vtable; backends provide concrete byte-pipe impls)
//      │
//   {loopback, Spoor-over-Unix-socket, future stratumd-backed Spoor, ...}
//
// Responsibilities of this layer:
//
//   1. Frame discipline. Outbound: caller hands in a complete framed
//      Tmsg (the session module produces this); the transport calls
//      ops->send once with the full frame. Inbound: the transport
//      receives bytes from ops->recv, aggregating partial reads until
//      a complete frame is assembled (signaled by peeking the
//      header.size field after the first 7 bytes are in hand).
//
//   2. Receive-buffer ownership. The caller provides a fixed-size
//      buffer at init time (sized to the session's negotiated msize).
//      The transport never allocates; it fills the caller buffer.
//
//   3. State machine: INIT → OPEN → CLOSED (+ ERROR sink).
//
//   4. Session composition. `p9_transport_exchange` composes a session
//      Tmsg-send + transport.send + transport.recv + session.dispatch
//      into one call. Tests + future client wrapper use it.
//
// Spec posture:
//
//   No new TLA+ spec at v1.0. The transport adds three properties:
//     - FramingIntegrity: bytes arrive in complete-message units (the
//       partial-read aggregation makes this structural).
//     - RequestResponseOrdering: the loopback enforces strict 1:1 pair
//       (every send produces exactly one response); future async
//       backends will use the session's tag-indexed dispatch to remain
//       correct under out-of-order arrival.
//     - NoMessageLoss: every send is matched by a recv (liveness
//       property; depends on the responsive-server assumption).
//
//   The session's invariants (I-10 tag uniqueness, I-11 fid stability,
//   FlowControl, OutOfOrderCorrectness) compose unchanged through the
//   transport.
//
// Concurrency: the transport is NOT thread-safe internally. Callers
// must serialize. At v1.0 the kernel 9P client serializes per-Proc via
// the Proc lock (matching the session's per-session-serialized model).

#ifndef THYLACINE_9P_TRANSPORT_H
#define THYLACINE_9P_TRANSPORT_H

#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

struct p9_session;
struct p9_dispatch_result;

// =============================================================================
// State machine.
// =============================================================================

enum p9_transport_state {
    P9_TRANS_INIT   = 0,   // before first I/O
    P9_TRANS_OPEN   = 1,   // active
    P9_TRANS_CLOSED = 2,   // explicit close; no further I/O
    P9_TRANS_ERROR  = 3,   // sink on first I/O failure; no further I/O
};

// =============================================================================
// Backend vtable.
//
// Backends provide raw byte read/write semantics (standard read(2) /
// write(2) shape). The transport core composes them into frame-aware
// recv/send.
// =============================================================================

struct p9_transport_ops {
    // Write `len` bytes from `buf`. Returns the number of bytes
    // written (must equal `len` on success — backends loop internally
    // if their underlying pipe accepts shorter writes), 0 on closed
    // pipe, or -1 on error.
    int (*send)(void *ctx, const u8 *buf, size_t len);

    // Read up to `cap` bytes into `buf`. Returns the number of bytes
    // read (1..cap), 0 on EOF, or -1 on error. May return fewer bytes
    // than requested — the transport core loops to aggregate a full
    // frame.
    int (*recv)(void *ctx, u8 *buf, size_t cap);

    // Close the underlying pipe. Returns 0 on success, -1 on error.
    // Called from p9_transport_close; never reaches the backend after
    // the transport has transitioned to CLOSED.
    int (*close)(void *ctx);

    // Arm or disarm a deadline for the NEXT blocking recv (absolute ns;
    // 0 = no deadline = block indefinitely). NULL-permitted: a backend
    // with no deadline mechanism leaves this NULL, and the deadline-aware
    // reader pump simply blocks (it never observes the idle return).
    // Arming also clears the recv_timed_out signal (mirrors
    // srvconn_set_client_deadline). Loom-4 (SQPOLL) uses this to make the
    // poll-thread's reader recv frame-boundary-interruptible without
    // desyncing the byte stream (LOOM.md §8.6).
    void (*set_recv_deadline)(void *ctx, u64 deadline_ns);

    // True iff the MOST RECENT recv returned <= 0 because the armed
    // deadline lapsed (vs EOF / error / death-interrupt). NULL-permitted
    // (treated as false). Read it BEFORE the next set_recv_deadline --
    // arming resets the signal.
    bool (*recv_timed_out)(void *ctx);

    // Opaque pointer the backend uses for its state (e.g., a pointer
    // to a `struct p9_loopback`).
    void *ctx;
};

// =============================================================================
// Transport struct. Caller-allocated.
// =============================================================================

#define P9_TRANSPORT_MAGIC  0x50395452u   // "P9TR" little-endian

struct p9_transport {
    u32                      magic;
    enum p9_transport_state  state;
    struct p9_transport_ops  ops;
    u8                      *recv_buf;     // caller-provided
    size_t                   recv_cap;     // bytes
    size_t                   last_recv_len;// length of the most recent recv'd frame
    // Diagnostics.
    u32                      total_sent;
    u32                      total_recvd;
    u32                      total_errors;
};

// =============================================================================
// Lifecycle.
// =============================================================================

// Initialize a transport. `ops` is the backend vtable. `recv_buf` /
// `recv_cap` is the caller-allocated frame buffer (sized to msize).
// Returns 0 on success, -1 on arg violation. Transitions to OPEN.
int  p9_transport_init(struct p9_transport *t,
                        struct p9_transport_ops ops,
                        u8 *recv_buf, size_t recv_cap);

// Tear down: clobbers magic, transitions to CLOSED. Does NOT call the
// backend's close (use p9_transport_close for graceful shutdown).
void p9_transport_destroy(struct p9_transport *t);

// Graceful close: calls ops->close, then transitions to CLOSED.
int  p9_transport_close(struct p9_transport *t);

// =============================================================================
// I/O.
// =============================================================================

// Transient send back-pressure (#349). A backend whose outbound buffer is
// momentarily full but ALIVE returns this from ops.send, and p9_transport_send
// propagates it -- distinct from a genuine break (<0) and from success (0). The
// caller (client_run's flow control) drains the reply path + retries; it is NOT
// a session death. Only the SrvConn c2s ring backend produces it (a transiently-
// full ring under #841 pipelining); single-frame backends never do. The value
// is outside {0, -1, any byte count} so no backend collides with it.
#define P9_TRANSPORT_EAGAIN  (-11)

// Send one complete framed Tmsg. `msg` is the bytes the session module
// produced. `len` is the total frame length (including the 7-byte
// header; equals header.size). Returns 0 on success, -1 on error
// (validates frame consistency before calling the backend; transitions
// to ERROR on backend failure), or P9_TRANSPORT_EAGAIN on transient
// all-or-nothing back-pressure (the whole frame did not fit; retryable).
int  p9_transport_send(struct p9_transport *t,
                        const u8 *msg, size_t len);

// Receive one complete framed Rmsg into t->recv_buf. Returns the
// frame length on success (also stored in t->last_recv_len), -1 on
// error (truncation, frame > recv_cap, backend failure). Transitions
// to ERROR on backend failure.
int  p9_transport_recv(struct p9_transport *t);

// NULL-safe shims over the optional deadline vtable ops. A backend that
// leaves set_recv_deadline / recv_timed_out NULL gets the no-deadline
// behavior (arming is a no-op; timed_out is always false). Neither shim
// touches the transport state machine -- they are safe to call from the
// frame-aware reader that bypasses p9_transport_recv's ERROR latch.
void p9_transport_set_recv_deadline(struct p9_transport *t, u64 deadline_ns);
bool p9_transport_recv_timed_out(const struct p9_transport *t);

// Convenience: send a request, then receive the response. Equivalent
// to p9_transport_send + p9_transport_recv. Returns the response
// length on success, -1 on error. The response bytes are in
// t->recv_buf[0..return_value).
int  p9_transport_round_trip(struct p9_transport *t,
                              const u8 *request, size_t request_len);

// Compose with session: take a built Tmsg + run send + recv + session
// dispatch in one call. The session must have already produced
// `request_msg` via one of its send_* functions. After the round trip,
// the session's dispatch_rmsg is called on the received bytes; the
// result is surfaced in `out`. Returns 0 on success, -1 on error.
int  p9_transport_exchange(struct p9_transport *t,
                            struct p9_session *s,
                            const u8 *request_msg, size_t request_len,
                            struct p9_dispatch_result *out);

// =============================================================================
// Query helpers.
// =============================================================================

bool   p9_transport_is_open(const struct p9_transport *t);
size_t p9_transport_last_recv_len(const struct p9_transport *t);

#endif  // THYLACINE_9P_TRANSPORT_H
