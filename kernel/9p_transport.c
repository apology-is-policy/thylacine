// 9P2000.L transport layer — P5-transport.
//
// Per `kernel/include/thylacine/9p_transport.h`. Frame-aware byte pipe
// composed of a backend vtable + a caller-provided receive buffer.

#include <thylacine/9p_transport.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

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
