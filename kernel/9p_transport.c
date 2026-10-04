// 9P2000.L transport layer — P5-transport.
//
// Per `kernel/include/thylacine/9p_transport.h`. Frame-aware byte pipe
// composed of a backend vtable + a caller-provided receive buffer.

#include <thylacine/9p_transport.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>
#include <thylacine/errno.h>

// =============================================================================
// Compile-time invariants.
// =============================================================================

_Static_assert(P9_TRANSPORT_MAGIC == 0x50395452u, "transport magic drift");

// =============================================================================
// Lifecycle.
// =============================================================================

int p9_transport_init(struct p9_transport *t,
                       struct p9_transport_ops ops,
                       u8 *recv_buf, size_t recv_cap) {
    if (!t) return -1;
    if (!ops.send || !ops.recv || !ops.close) return -1;
    if (!recv_buf) return -1;
    if (recv_cap < P9_HDR_LEN) return -1;  // need room for at least a header
    t->magic         = P9_TRANSPORT_MAGIC;
    t->state         = P9_TRANS_OPEN;
    t->ops           = ops;
    t->recv_buf      = recv_buf;
    t->recv_cap      = recv_cap;
    t->last_recv_len = 0;
    t->total_sent    = 0;
    t->total_recvd   = 0;
    t->total_errors  = 0;
    return 0;
}

void p9_transport_destroy(struct p9_transport *t) {
    if (!t) return;
    if (t->magic != P9_TRANSPORT_MAGIC) return;
    // Clobber magic first so subsequent calls fast-fail (R9 F148 mirror —
    // see docs/reference/39-hw-handles.md caveat #2 for the kobj_*_unref
    // pattern; mirrored in 9p_session.c).
    t->magic         = 0;
    t->state         = P9_TRANS_CLOSED;
    t->recv_buf      = NULL;
    t->recv_cap      = 0;
    t->last_recv_len = 0;
}

int p9_transport_close(struct p9_transport *t) {
    if (!t) return -1;
    if (t->magic != P9_TRANSPORT_MAGIC) return -1;
    if (t->state == P9_TRANS_CLOSED) return 0;       // idempotent
    // A private driver must abort under its owner lock before dropping backend
    // storage. A legacy close cannot bypass the cursor's terminal latch.
    if (t->state == P9_TRANS_PROGRESS) return -1;
    int rc = t->ops.close(t->ops.ctx);
    t->state = P9_TRANS_CLOSED;
    return rc;
}

// =============================================================================
// Internal: full write. Backends are expected to satisfy the full
// request, but defense-in-depth loops on short writes.
// =============================================================================

static int do_send(struct p9_transport *t, const u8 *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = t->ops.send(t->ops.ctx, buf + sent, len - sent);
        if (n == P9_TRANSPORT_EAGAIN) {
            // #349: transient all-or-nothing back-pressure (a transiently-full
            // SrvConn c2s ring under #841 pipelining). Valid ONLY before any
            // byte of this frame is on the wire (sent == 0); a mid-frame EAGAIN
            // would strand a fragment + desync the shared stream, so treat that
            // as fatal. Propagate the retryable signal to client_run's flow
            // control -- do NOT mark the transport ERROR (the ring is alive).
            if (sent == 0) return P9_TRANSPORT_EAGAIN;
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        if (n <= 0) {
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        if ((size_t)n > len - sent) {
            // Backend bug: claimed to write more than we asked.
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// =============================================================================
// Internal: aggregate one complete frame into t->recv_buf.
//
// Strategy: read the 7-byte header first. Peek size. Then read body
// until size bytes are in hand. Loop on short reads.
//
// Reject:
//   - EOF before a complete header (truncation)
//   - header.size < P9_HDR_LEN (impossible header)
//   - header.size > t->recv_cap (frame won't fit)
//   - EOF before a complete body (truncation mid-frame)
// =============================================================================

static int do_recv(struct p9_transport *t) {
    // Phase 1: read header.
    size_t got = 0;
    while (got < P9_HDR_LEN) {
        int n = t->ops.recv(t->ops.ctx, t->recv_buf + got,
                             P9_HDR_LEN - got);
        if (n <= 0) {
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        if ((size_t)n > P9_HDR_LEN - got) {
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        got += (size_t)n;
    }

    // Peek size to learn the body length.
    u32 size; u8 type; u16 tag;
    int peek_rc = p9_peek_header(t->recv_buf, got, &size, &type, &tag);
    if (peek_rc < 0) {
        t->state         = P9_TRANS_ERROR;
        t->total_errors++;
        return -1;
    }
    if (size < P9_HDR_LEN) {
        t->state         = P9_TRANS_ERROR;
        t->total_errors++;
        return -1;
    }
    if ((size_t)size > t->recv_cap) {
        t->state         = P9_TRANS_ERROR;
        t->total_errors++;
        return -1;
    }

    // Phase 2: read body.
    while (got < (size_t)size) {
        int n = t->ops.recv(t->ops.ctx, t->recv_buf + got,
                             (size_t)size - got);
        if (n <= 0) {
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        if ((size_t)n > (size_t)size - got) {
            t->state         = P9_TRANS_ERROR;
            t->total_errors++;
            return -1;
        }
        got += (size_t)n;
    }

    t->last_recv_len = got;
    return (int)got;
}

// =============================================================================
// Public I/O.
// =============================================================================

int p9_transport_send(struct p9_transport *t,
                       const u8 *msg, size_t len) {
    if (!t) return -1;
    if (t->magic != P9_TRANSPORT_MAGIC) return -1;
    if (t->state != P9_TRANS_OPEN) return -1;
    if (!msg) return -1;
    if (len < P9_HDR_LEN) return -1;

    // Validate: outbound frame's header.size must match the caller's len.
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(msg, len, &size, &type, &tag) < 0) return -1;
    if ((size_t)size != len) return -1;

    int rc = do_send(t, msg, len);
    if (rc == P9_TRANSPORT_EAGAIN) return P9_TRANSPORT_EAGAIN;  // #349: retryable
    if (rc < 0) return -1;
    t->total_sent++;
    return 0;
}

int p9_transport_recv(struct p9_transport *t) {
    if (!t) return -1;
    if (t->magic != P9_TRANSPORT_MAGIC) return -1;
    if (t->state != P9_TRANS_OPEN) return -1;

    int rc = do_recv(t);
    if (rc < 0) return -1;
    t->total_recvd++;
    return rc;
}

int p9_transport_round_trip(struct p9_transport *t,
                              const u8 *request, size_t request_len) {
    int rc = p9_transport_send(t, request, request_len);
    if (rc < 0) return -1;
    return p9_transport_recv(t);
}

int p9_transport_exchange(struct p9_transport *t,
                            struct p9_session *s,
                            const u8 *request_msg, size_t request_len,
                            struct p9_dispatch_result *out) {
    if (!t || !s || !out) return -1;
    int recv_len = p9_transport_round_trip(t, request_msg, request_len);
    if (recv_len < 0) return -1;
    return p9_session_dispatch_rmsg(s, t->recv_buf, (size_t)recv_len, out);
}

// =============================================================================
// Query helpers.
// =============================================================================

bool p9_transport_is_open(const struct p9_transport *t) {
    if (!t) return false;
    if (t->magic != P9_TRANSPORT_MAGIC) return false;
    return t->state == P9_TRANS_OPEN;
}

size_t p9_transport_last_recv_len(const struct p9_transport *t) {
    if (!t) return 0;
    if (t->magic != P9_TRANSPORT_MAGIC) return 0;
    return t->last_recv_len;
}

void p9_transport_set_recv_deadline(struct p9_transport *t, u64 deadline_ns) {
    if (!t || t->magic != P9_TRANSPORT_MAGIC) return;
    if (t->ops.set_recv_deadline)
        t->ops.set_recv_deadline(t->ops.ctx, deadline_ns);
}

bool p9_transport_recv_timed_out(const struct p9_transport *t) {
    if (!t || t->magic != P9_TRANSPORT_MAGIC) return false;
    if (t->ops.recv_timed_out)
        return t->ops.recv_timed_out(t->ops.ctx);
    return false;
}

// =============================================================================
// AS-1: resumable frame discipline for an exclusively owned private transport.
// There is no loop across a backend call. Cancellation and the progress driver
// share an enclosing lock; cancellation never waits for the peer or consumes a
// CQ entry. The cursor retains partial framing across EAGAIN. A terminal fault
// latches the entire private stream, not an individual tag, before any reuse.
// Legacy transport functions above keep their existing semantics.
// =============================================================================

static bool progress_live(const struct p9_transport_progress *p) {
    return p && !p->aborted && p->transport &&
           p->transport->magic == P9_TRANSPORT_MAGIC &&
           p->transport->state == P9_TRANS_PROGRESS;
}

int p9_transport_progress_init(struct p9_transport_progress *p,
                              struct p9_transport *t,
                              struct p9_transport_try_ops ops,
                              size_t frame_limit) {
    if (!p || !t || t->magic != P9_TRANSPORT_MAGIC ||
        t->state != P9_TRANS_OPEN || t->total_sent || t->total_recvd ||
        !ops.send || !ops.recv || !ops.abort ||
        frame_limit < P9_HDR_LEN || frame_limit > t->recv_cap ||
        frame_limit > 0x7fffffffu) return -1;
    *p = (struct p9_transport_progress){
        .transport = t, .ops = ops, .rx_goal = P9_HDR_LEN,
        .frame_limit = frame_limit,
    };
    t->state = P9_TRANS_PROGRESS;
    return 0;
}

int p9_transport_progress_limit(struct p9_transport_progress *p, size_t limit) {
    if (!progress_live(p) || p->tx || p->rx_have ||
        limit < P9_HDR_LEN || limit > p->frame_limit) return -1;
    p->frame_limit = limit;
    return 0;
}

void p9_transport_progress_abort(struct p9_transport_progress *p) {
    if (!p || p->aborted || !p->transport) return;
    p->aborted = true;
    p->transport->state = P9_TRANS_ERROR;
    p->transport->total_errors++;
    // No storage release here: the enclosing owner's borrow/pin accounting
    // determines actual retirement after this serialized call returns.
    p->tx = NULL;
    p->tx_len = p->tx_sent = p->rx_have = 0;
    p->rx_goal = P9_HDR_LEN;
    p->ops.abort(p->ops.ctx);
}

int p9_transport_progress_queue(struct p9_transport_progress *p,
                               const u8 *msg, size_t len) {
    if (!progress_live(p) || p->tx || !msg ||
        len < P9_HDR_LEN || len > p->frame_limit) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(msg, len, &size, &type, &tag) < 0 || size != len)
        return -1;
    p->tx = msg;
    p->tx_len = len;
    p->tx_sent = 0;
    return 0;
}

int p9_transport_progress_send(struct p9_transport_progress *p) {
    if (!progress_live(p) || !p->tx) return -1;
    size_t left = p->tx_len - p->tx_sent;
    int n = p->ops.send(p->ops.ctx, p->tx + p->tx_sent, left);
    if (n == P9_TRANSPORT_EAGAIN) return 0;
    // Even a backend's invalid oversize result cannot promise no bytes escaped.
    if (n > 0) p->bytes_sent = true;
    if (n <= 0 || (size_t)n > left) {
        p9_transport_progress_abort(p);
        return -1;
    }
    p->tx_sent += (size_t)n;
    if (p->tx_sent < p->tx_len) return 0;
    p->transport->total_sent++;
    p->tx = NULL;
    p->tx_len = p->tx_sent = 0;
    return 1;
}

int p9_transport_progress_recv(struct p9_transport_progress *p) {
    if (!progress_live(p)) return -1;
    struct p9_transport *t = p->transport;
    size_t left = p->rx_goal - p->rx_have;
    int n = p->ops.recv(p->ops.ctx, t->recv_buf + p->rx_have, left);
    if (n == P9_TRANSPORT_EAGAIN) return 0;
    if (n <= 0 || (size_t)n > left) {
        p9_transport_progress_abort(p);
        return -1;
    }
    p->rx_have += (size_t)n;
    if (p->rx_have < p->rx_goal) return 0;
    if (p->rx_goal == P9_HDR_LEN) {
        u32 size; u8 type; u16 tag;
        if (p9_peek_header(t->recv_buf, P9_HDR_LEN, &size, &type, &tag) < 0 ||
            size < P9_HDR_LEN || size > p->frame_limit) {
            p9_transport_progress_abort(p);
            return -1;
        }
        p->rx_goal = size;
        if (p->rx_have < p->rx_goal) return 0;
    }
    int len = (int)p->rx_have;
    t->last_recv_len = (size_t)len;
    t->total_recvd++;
    p->rx_have = 0;
    p->rx_goal = P9_HDR_LEN;
    return len;
}

// Composition, like the synchronous exchange above, reuses the session's
// version validation, tag matching and fid binding. No second 9P state machine.
static int handshake_fail(struct p9_handshake_progress *h, int reason) {
    if (h->phase != P9_HS_FAILED) {
        h->failed_phase = h->phase;
        h->phase = P9_HS_FAILED;
        h->reason = reason;
        p9_transport_progress_abort(h->progress);
        // Keep storage and tag bookkeeping until the owner retires it. Closing
        // the state forbids any subsequent builder without requiring a reply.
        h->session->state = P9_SESS_CLOSED;
    }
    return h->reason;
}

int p9_handshake_progress_init(struct p9_handshake_progress *h,
                              struct p9_session *s,
                              struct p9_transport_progress *p,
                              u8 *out, size_t out_cap,
                              u32 principal, u64 deadline_ns) {
    if (!h || !s || s->magic != P9_SESSION_MAGIC || s->state != P9_SESS_INIT ||
        !progress_live(p) || p->tx || p->rx_have ||
        p->frame_limit != s->msize || !out || !deadline_ns ||
        p9_session_inflight(s) || s->total_sent || p->transport->total_sent ||
        p->transport->total_recvd) return -T_E_INVAL;
    *h = (struct p9_handshake_progress){
        .session = s, .progress = p, .out = out, .out_cap = out_cap,
        .principal = principal, .deadline_ns = deadline_ns,
        .phase = P9_HS_VERSION_SEND,
    };
    size_t cap = out_cap < p->frame_limit ? out_cap : p->frame_limit;
    int n = p9_session_send_version(s, out, cap, NULL, 0);
    if (n < 0 || p9_transport_progress_queue(p, out, (size_t)n) < 0)
        return handshake_fail(h, -T_E_IO);
    return 0;
}

void p9_handshake_progress_abort(struct p9_handshake_progress *h) {
    if (h && h->session && h->progress) (void)handshake_fail(h, -T_E_CANCELED);
}

int p9_handshake_progress_step(struct p9_handshake_progress *h, u64 now_ns) {
    if (!h || !h->session || !h->progress) return -T_E_INVAL;
    if (h->phase == P9_HS_FAILED) return h->reason;
    if (!progress_live(h->progress)) return handshake_fail(h, -T_E_CANCELED);
    if (h->phase == P9_HS_READY) return 1;
    if (now_ns >= h->deadline_ns) return handshake_fail(h, -T_E_TIMEDOUT);
    if (h->phase == P9_HS_VERSION_SEND || h->phase == P9_HS_ATTACH_SEND) {
        int rc = p9_transport_progress_send(h->progress);
        if (rc < 0) return handshake_fail(h, -T_E_IO);
        if (rc > 0) h->phase = h->phase == P9_HS_VERSION_SEND
                               ? P9_HS_VERSION_RECV : P9_HS_ATTACH_RECV;
        return 0;
    }
    int len = p9_transport_progress_recv(h->progress);
    if (len < 0) return handshake_fail(h, -T_E_IO);
    if (!len) return 0;
    struct p9_dispatch_result r;
    if (p9_session_dispatch_rmsg(h->session,
            h->progress->transport->recv_buf, (size_t)len, &r) < 0)
        return handshake_fail(h, -T_E_IO);
    if (r.is_error) {
        // Same hostile-ecode bound as the shared client's map_error; never
        // negate a peer-controlled u32 before validating the signed range.
        int reason = r.ecode && r.ecode <= 4095u ? -(int)r.ecode : -T_E_IO;
        return handshake_fail(h, reason);
    }
    if (h->phase == P9_HS_VERSION_RECV) {
        if (r.kind != P9_TVERSION || h->session->state != P9_SESS_VERSIONED ||
            p9_transport_progress_limit(h->progress,
                h->session->negotiated_msize) < 0)
            return handshake_fail(h, -T_E_IO);
        size_t cap = h->out_cap < h->progress->frame_limit
                     ? h->out_cap : h->progress->frame_limit;
        int n = p9_session_send_attach(h->session, h->out, cap,
                                      NULL, 0, NULL, 0, h->principal);
        if (n < 0 || p9_transport_progress_queue(h->progress, h->out, (size_t)n) < 0)
            return handshake_fail(h, -T_E_IO);
        h->phase = P9_HS_ATTACH_SEND;
        return 0;
    }
    if (h->phase != P9_HS_ATTACH_RECV || r.kind != P9_TATTACH ||
        !p9_session_is_open(h->session) ||
        !p9_session_fid_bound(h->session, h->session->root_fid))
        return handshake_fail(h, -T_E_IO);
    h->phase = P9_HS_READY;
    return 1;
}
