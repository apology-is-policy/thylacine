// 9P multi-queue loopback transport (Loom-6c test scaffold).
//
// The single-slot `p9_loopback` (9p_transport_loopback.h) refuses a second send
// while a prior response is undrained -- it models a strict synchronous
// request/reply, so it CANNOT hold more than one 9P RPC in flight. That is fine
// for the synchronous p9_client_* API, but it makes multi-in-flight async
// dispatch (the Loom elected-reader path: submit N ops, then demux N replies)
// untestable -- the exact coverage gap the Loom audits carried since #841.
//
// This transport closes that gap. It is a byte-stream FIFO (like a real srvconn
// byte ring, NOT a single slot): every `send` synthesizes a reply (via the same
// responder type as p9_loopback) and APPENDS it to the ring; `recv` drains the
// ring front. So N async submits stage N replies, and the elected reader demuxes
// them frame by frame -- genuine multi-in-flight over a deterministic transport.
//
// Single-producer/single-consumer by construction in the deterministic tests
// (submit-all-then-drain), but every access takes `lock` so a future concurrent
// driver (the Loom-6d native multi-thread harness) can share one transport. recv
// is NON-blocking: an empty ring returns 0 (EOF) exactly like p9_loopback, so a
// deterministic test that submits N and completes N never over-pumps into EOF.

#ifndef THYLACINE_9P_TRANSPORT_MQ_H
#define THYLACINE_9P_TRANSPORT_MQ_H

#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_loopback.h>   // p9_loopback_responder
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#define P9_MQ_LOOPBACK_MAGIC  0x4D514C30u   // "MQL0" little-endian
#define P9_MQ_RING_CAP        8192u         // byte-ring capacity (>> N small frames)
#define P9_MQ_SCRATCH_CAP     1024u         // per-send reply synthesis scratch

struct p9_mq_loopback {
    u32                    magic;
    p9_loopback_responder  responder;   // synthesizes each reply (reused type)
    void                  *user_ctx;
    spin_lock_t            lock;        // serializes send/recv (future concurrent driver)
    bool                   closed;
    // Linear byte FIFO: send appends at `tail`, recv drains from `head`; both
    // reset to 0 when the ring fully drains (head == tail), so submit/drain cycles
    // never run off the end. A reply that would overflow [tail, CAP) fails the send.
    u8                     ring[P9_MQ_RING_CAP];
    u32                    head;        // next byte to recv
    u32                    tail;        // next free byte
    u8                     scratch[P9_MQ_SCRATCH_CAP];   // per-send synthesis
    u64                    sends;
    u64                    recvs;
    // Deadline knob (parity with p9_loopback): an armed deadline on an empty ring
    // returns -1 + sets timed_out (the frame-boundary-timeout model) instead of 0.
    bool                   deadline_armed;
    bool                   timed_out;
    // Back-pressure knob (#349 regression). A test sets eagain_budget = K to make
    // the next K send calls return P9_TRANSPORT_EAGAIN (a transiently-full-but-ALIVE
    // c2s ring) and REJECT the frame -- no reply synthesized, no acceptance --
    // exactly like a real full ring. The reply is generated only when the frame is
    // accepted (budget exhausted), so a prior op's already-queued reply stays
    // drainable: the production shape where client_send_flow's self-pump drains an
    // OTHER op's reply, frees the slot, then the rejected frame's retry is accepted.
    u32                    eagain_budget;
};

// Initialize. `responder` is required (same contract as p9_loopback_responder:
// inspect the request, write the reply into the caller-supplied buffer, return
// its length or -1). user_ctx may be NULL.
int  p9_mq_loopback_init(struct p9_mq_loopback *mq,
                         p9_loopback_responder responder, void *user_ctx);
void p9_mq_loopback_destroy(struct p9_mq_loopback *mq);

// Build a transport_ops vtable that delegates to this transport.
struct p9_transport_ops p9_mq_loopback_ops_for(struct p9_mq_loopback *mq);

#endif  // THYLACINE_9P_TRANSPORT_MQ_H
