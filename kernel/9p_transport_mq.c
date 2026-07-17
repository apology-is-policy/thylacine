// 9P multi-queue loopback transport (Loom-6c test scaffold). See the header for
// the rationale: a byte-stream FIFO that holds N replies in flight, so the Loom
// elected-reader path can be driven multi-in-flight (the coverage gap the Loom
// audits carried since #841).

#include <thylacine/9p_transport_mq.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

_Static_assert(P9_MQ_LOOPBACK_MAGIC == 0x4D514C30u, "mq loopback magic drift");

int p9_mq_loopback_init(struct p9_mq_loopback *mq,
                        p9_loopback_responder responder, void *user_ctx) {
    if (!mq || !responder) return -1;
    mq->magic          = P9_MQ_LOOPBACK_MAGIC;
    mq->responder      = responder;
    mq->user_ctx       = user_ctx;
    spin_lock_init(&mq->lock);
    mq->closed         = false;
    mq->head           = 0;
    mq->tail           = 0;
    mq->sends          = 0;
    mq->recvs          = 0;
    mq->deadline_armed = false;
    mq->timed_out      = false;
    mq->eagain_budget  = 0;
    mq->scribble_buf   = NULL;
    mq->scribble_len   = 0;
    mq->scribble_arm   = 0;
    return 0;
}

void p9_mq_loopback_destroy(struct p9_mq_loopback *mq) {
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return;
    mq->magic  = 0;
    mq->head   = 0;
    mq->tail   = 0;
    mq->closed = true;
}

// send: synthesize the reply (into scratch, under the lock -- the responder is
// non-sleeping test code) and APPEND it to the FIFO. Returns `len` (the whole
// request was consumed) like p9_loopback; -1 on a closed transport, a responder
// refusal, or a ring overflow (a well-sized test never overflows: frames are
// tiny and the ring resets to 0 on full drain).
static int mq_send(void *ctx, const u8 *buf, size_t len) {
    struct p9_mq_loopback *mq = (struct p9_mq_loopback *)ctx;
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return -1;
    spin_lock(&mq->lock);
    if (mq->closed) { spin_unlock(&mq->lock); return -1; }
    if (mq->eagain_budget > 0) {            // #349: transiently-full-but-ALIVE ring
        mq->eagain_budget--;                // reject the frame; do NOT synthesize a
        spin_unlock(&mq->lock);             // reply (a real full ring drops nothing
        return P9_TRANSPORT_EAGAIN;         // on the floor -- the sender retries)
    }
    int n = mq->responder(mq->user_ctx, buf, len, mq->scratch, sizeof(mq->scratch));
    if (n < 0 || (size_t)n > sizeof(mq->scratch)) { spin_unlock(&mq->lock); return -1; }
    if ((size_t)mq->tail + (size_t)n > P9_MQ_RING_CAP) {   // would overflow the ring
        spin_unlock(&mq->lock);
        return -1;
    }
    for (int i = 0; i < n; i++) mq->ring[mq->tail + (u32)i] = mq->scratch[i];
    mq->tail += (u32)n;
    mq->sends++;
    spin_unlock(&mq->lock);
    return (int)len;
}

// recv: drain up to `cap` bytes from the FIFO front (a contiguous run -- the ring
// is linear, [head,tail) is always contiguous; the client's frame assembler
// loops). On a fully-drained ring reset head/tail to 0 so submit/drain cycles
// never run off the end. Empty: an armed deadline returns -1 + timed_out (the
// frame-boundary-timeout model); otherwise 0 (EOF), matching p9_loopback so a
// test that completes exactly N never over-pumps into a spurious death.
static int mq_recv(void *ctx, u8 *buf, size_t cap) {
    struct p9_mq_loopback *mq = (struct p9_mq_loopback *)ctx;
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return -1;
    spin_lock(&mq->lock);
    if (mq->closed) { spin_unlock(&mq->lock); return -1; }
    // #375 clobber knob: this recv runs inside the client's pump/park window
    // (c->lock dropped), where a peer may legally rebuild the shared out_buf.
    // Model that peer write here, deterministically.
    if (mq->scribble_arm > 0 && mq->scribble_buf && mq->scribble_len > 0) {
        mq->scribble_arm--;
        for (u32 i = 0; i < mq->scribble_len; i++) mq->scribble_buf[i] = 0x5A;
    }
    u32 avail = mq->tail - mq->head;
    if (avail == 0) {
        if (mq->deadline_armed) { mq->timed_out = true; spin_unlock(&mq->lock); return -1; }
        spin_unlock(&mq->lock);
        return 0;                       // EOF
    }
    u32 to_copy = (avail < cap) ? avail : (u32)cap;
    for (u32 i = 0; i < to_copy; i++) buf[i] = mq->ring[mq->head + i];
    mq->head += to_copy;
    if (mq->head == mq->tail) { mq->head = 0; mq->tail = 0; }   // drained -> reset
    mq->recvs++;
    spin_unlock(&mq->lock);
    return (int)to_copy;
}

static int mq_close(void *ctx) {
    struct p9_mq_loopback *mq = (struct p9_mq_loopback *)ctx;
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return -1;
    spin_lock(&mq->lock);
    mq->closed = true;
    spin_unlock(&mq->lock);
    return 0;
}

static void mq_set_recv_deadline(void *ctx, u64 deadline_ns) {
    struct p9_mq_loopback *mq = (struct p9_mq_loopback *)ctx;
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return;
    spin_lock(&mq->lock);
    mq->deadline_armed = (deadline_ns != 0);
    mq->timed_out      = false;
    spin_unlock(&mq->lock);
}

static bool mq_recv_timed_out(void *ctx) {
    struct p9_mq_loopback *mq = (struct p9_mq_loopback *)ctx;
    if (!mq || mq->magic != P9_MQ_LOOPBACK_MAGIC) return false;
    return mq->timed_out;
}

struct p9_transport_ops p9_mq_loopback_ops_for(struct p9_mq_loopback *mq) {
    struct p9_transport_ops ops;
    ops.send              = mq_send;
    ops.recv              = mq_recv;
    ops.close             = mq_close;
    ops.set_recv_deadline = mq_set_recv_deadline;
    ops.recv_timed_out    = mq_recv_timed_out;
    ops.ctx               = mq;
    return ops;
}
