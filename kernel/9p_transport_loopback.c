// 9P transport loopback backend — P5-transport, test scaffold.

#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

_Static_assert(P9_LOOPBACK_MAGIC == 0x4C424B30u, "loopback magic drift");

int p9_loopback_init(struct p9_loopback *lb,
                      u8 *response_buf, size_t response_cap,
                      p9_loopback_responder responder, void *user_ctx) {
    if (!lb) return -1;
    if (!responder) return -1;
    if (!response_buf) return -1;
    if (response_cap < P9_HDR_LEN) return -1;
    lb->magic        = P9_LOOPBACK_MAGIC;
    lb->responder    = responder;
    lb->user_ctx     = user_ctx;
    lb->response     = response_buf;
    lb->response_cap = response_cap;
    lb->response_len = 0;
    lb->response_pos = 0;
    lb->chunk_size     = 0;
    lb->sends          = 0;
    lb->recvs          = 0;
    lb->closed         = false;
    lb->deadline_armed = false;
    lb->timed_out      = false;
    return 0;
}

void p9_loopback_destroy(struct p9_loopback *lb) {
    if (!lb) return;
    if (lb->magic != P9_LOOPBACK_MAGIC) return;
    lb->magic        = 0;
    lb->responder    = NULL;
    lb->user_ctx     = NULL;
    lb->response_len = 0;
    lb->response_pos = 0;
    lb->closed       = true;
}

void p9_loopback_set_chunk_size(struct p9_loopback *lb, size_t chunk_size) {
    if (!lb) return;
    if (lb->magic != P9_LOOPBACK_MAGIC) return;
    lb->chunk_size = chunk_size;
}

// =============================================================================
// Backend vtable thunks.
//
// send: calls the responder to synthesize a response, stages it.
// recv: drains the staged response in chunks per chunk_size.
// close: marks closed; subsequent send/recv return -1.
// =============================================================================

static int loopback_send(void *ctx, const u8 *buf, size_t len) {
    struct p9_loopback *lb = (struct p9_loopback *)ctx;
    if (!lb) return -1;
    if (lb->magic != P9_LOOPBACK_MAGIC) return -1;
    if (lb->closed) return -1;
    // Refuse if the loopback still has unread response bytes — the
    // test made a send without consuming the prior response, which
    // would lose data. (Real backends don't have this problem; this is
    // a test-side discipline check.)
    if (lb->response_pos < lb->response_len) return -1;
    // Call the responder to produce the response.
    int n = lb->responder(lb->user_ctx, buf, len,
                           lb->response, lb->response_cap);
    if (n < 0) return -1;
    if ((size_t)n > lb->response_cap) return -1;
    lb->response_len = (size_t)n;
    lb->response_pos = 0;
    lb->sends++;
    return (int)len;
}

static int loopback_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_loopback *lb = (struct p9_loopback *)ctx;
    if (!lb) return -1;
    if (lb->magic != P9_LOOPBACK_MAGIC) return -1;
    if (lb->closed) return -1;
    size_t available = lb->response_len - lb->response_pos;
    if (available == 0) {
        // Nothing staged. With a deadline armed, MODEL a frame-boundary
        // timeout (-1 + timed_out) so the deadline-aware reader pump can be
        // driven to its IDLE return deterministically; otherwise EOF.
        if (lb->deadline_armed) { lb->timed_out = true; return -1; }
        return 0;                       // EOF (the responder produced nothing)
    }
    size_t to_copy = (available < cap) ? available : cap;
    if (lb->chunk_size > 0 && to_copy > lb->chunk_size) {
        to_copy = lb->chunk_size;
    }
    for (size_t i = 0; i < to_copy; i++) {
        buf[i] = lb->response[lb->response_pos + i];
    }
    lb->response_pos += to_copy;
    lb->recvs++;
    return (int)to_copy;
}

static int loopback_close(void *ctx) {
    struct p9_loopback *lb = (struct p9_loopback *)ctx;
    if (!lb) return -1;
    if (lb->magic != P9_LOOPBACK_MAGIC) return -1;
    lb->closed = true;
    return 0;
}

static void loopback_set_recv_deadline(void *ctx, u64 deadline_ns) {
    struct p9_loopback *lb = (struct p9_loopback *)ctx;
    if (!lb) return;
    if (lb->magic != P9_LOOPBACK_MAGIC) return;
    lb->deadline_armed = (deadline_ns != 0);
    lb->timed_out      = false;          // arming/disarming clears the signal
}

static bool loopback_recv_timed_out(void *ctx) {
    struct p9_loopback *lb = (struct p9_loopback *)ctx;
    if (!lb) return false;
    if (lb->magic != P9_LOOPBACK_MAGIC) return false;
    return lb->timed_out;
}

struct p9_transport_ops p9_loopback_ops_for(struct p9_loopback *lb) {
    struct p9_transport_ops ops;
    ops.send              = loopback_send;
    ops.recv              = loopback_recv;
    ops.close             = loopback_close;
    ops.set_recv_deadline = loopback_set_recv_deadline;
    ops.recv_timed_out    = loopback_recv_timed_out;
    ops.ctx               = lb;
    return ops;
}
