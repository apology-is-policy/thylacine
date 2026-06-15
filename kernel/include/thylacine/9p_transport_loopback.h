// 9P transport loopback backend (P5-transport, test scaffold).
//
// In-memory test transport. Pairs every send with a caller-supplied
// responder function that inspects the request and synthesizes a
// response. Used by `kernel/test/test_9p_transport.c` + downstream
// integration tests to exercise the session + transport composition
// without an actual server.
//
// Design:
//
//   - send: copies the request into a staging buffer, calls the
//     responder to produce a response into the loopback's response
//     buffer.
//   - recv: returns chunks of the staged response (`chunk_size` bytes
//     per call, mimicking partial reads from a real stream socket).
//
// The `chunk_size` knob lets tests force partial-read aggregation
// through the transport core's `do_recv` loop. Default chunk_size = 0
// means "return the whole pending response in one call."
//
// The responder is stateful through `user_ctx`: tests use it to
// remember how many requests they've seen, vary the response per
// request, or inject errors.

#ifndef THYLACINE_9P_TRANSPORT_LOOPBACK_H
#define THYLACINE_9P_TRANSPORT_LOOPBACK_H

#include <thylacine/9p_transport.h>
#include <thylacine/types.h>

#define P9_LOOPBACK_MAGIC    0x4C424B30u   // "LBK0" little-endian

// Responder function. Inspects the request, writes the response into
// `response_buf` (max `response_cap` bytes), returns the response
// length on success, or -1 to simulate a backend error (the responder
// can refuse to answer).
typedef int (*p9_loopback_responder)(void *user_ctx,
                                       const u8 *request, size_t request_len,
                                       u8 *response_buf, size_t response_cap);

struct p9_loopback {
    u32                    magic;
    p9_loopback_responder  responder;
    void                  *user_ctx;
    // Caller-provided response staging buffer. Sized by the caller
    // (typically equal to the transport's recv_cap so a single round
    // trip never overflows the staging area).
    u8                    *response;
    size_t                 response_cap;
    size_t                 response_len;        // bytes currently staged
    size_t                 response_pos;        // bytes already drained
    // Optional limit on bytes per recv call (forces partial-read tests).
    // 0 means "no limit" (recv returns the whole staged remainder).
    size_t                 chunk_size;
    // Stats.
    u32                    sends;
    u32                    recvs;
    bool                   closed;
    // Deadline test knob (Loom-4). A real transport blocks recv on an
    // empty pipe until data / the deadline; the synchronous loopback has
    // no blocking, so it MODELS a frame-boundary deadline: when a deadline
    // is armed AND the staged response is empty, recv returns -1 + sets
    // `timed_out` (instead of 0 = EOF). Lets the deadline-aware reader-pump
    // tests drive the IDLE return deterministically without real time.
    bool                   deadline_armed;
    bool                   timed_out;       // last recv hit the armed deadline
};

// Initialize a loopback. `response_buf` / `response_cap` is the
// caller-allocated staging buffer for synthesized responses. The
// responder is required; user_ctx may be NULL.
int  p9_loopback_init(struct p9_loopback *lb,
                       u8 *response_buf, size_t response_cap,
                       p9_loopback_responder responder, void *user_ctx);
void p9_loopback_destroy(struct p9_loopback *lb);

// Set chunk_size: max bytes returned per p9_transport recv->ops.recv call.
// 0 = no limit. Used by partial-read tests.
void p9_loopback_set_chunk_size(struct p9_loopback *lb, size_t chunk_size);

// Clear any staged-but-unread response WITHOUT closing the transport: the next
// recv finds an empty ring and returns 0 = a CLEAN EOF (the peer/server endpoint
// closed cleanly). The peer-gone simulator -- distinct from p9_loopback_destroy,
// which closes the transport so recv returns -1 = a transport ERROR. The two map
// to the 9P client's two death reasons (clean EOF -> device-gone -P9_E_NODEV; an
// error -> generic -P9_E_IO). Tests the MENAGERIE.md section-10 device-gone leg.
void p9_loopback_force_eof(struct p9_loopback *lb);

// Build a transport_ops vtable that delegates to this loopback.
struct p9_transport_ops p9_loopback_ops_for(struct p9_loopback *lb);

#endif  // THYLACINE_9P_TRANSPORT_LOOPBACK_H
