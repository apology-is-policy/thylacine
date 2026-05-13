// 9P transport-layer unit tests (P5-transport).
//
// Covers:
//   - Lifecycle (init / destroy / close + magic clobber).
//   - Round-trip via loopback (send + recv).
//   - Frame validation on send + receive (size mismatch, frame > cap).
//   - Partial-read aggregation via the loopback's chunk_size knob.
//   - Backend-error propagation (transport transitions to ERROR; further
//     I/O refused).
//   - End-to-end composition with the session module — drives a full
//     handshake (Tversion + Tattach) through transport + dispatcher,
//     verifies state transitions and the result struct.

#include "test.h"

#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

void test_9p_transport_init_destroy(void);
void test_9p_transport_round_trip(void);
void test_9p_transport_send_frame_size_mismatch_rejected(void);
void test_9p_transport_recv_frame_too_large_rejected(void);
void test_9p_transport_partial_read_aggregation(void);
void test_9p_transport_backend_error_transitions_to_error(void);
void test_9p_transport_close_idempotent(void);
void test_9p_transport_exchange_drives_session_handshake(void);
void test_9p_transport_exchange_drives_session_walk(void);

// 4 KiB transport receive buffer + 4 KiB outbound staging buffer
// + 4 KiB loopback response staging.
static u8 g_recv_buf[4096];
static u8 g_send_buf[4096];
static u8 g_loopback_resp_buf[4096];

// =============================================================================
// Test responders.
//
// Each responder is a small function that inspects the request and
// produces a response. The loopback calls them on every send.
// =============================================================================

// A responder that echoes the request back as if it were the matching
// Rmsg — flips the type byte from Tx to Rx (Rx = Tx + 1 in 9P2000.L).
// Useful for testing the transport's framing without exercising real
// session semantics.
static int echo_flip_type_responder(void *ctx, const u8 *req, size_t req_len,
                                      u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len > resp_cap) return -1;
    for (size_t i = 0; i < req_len; i++) resp[i] = req[i];
    // Flip Tx → Rx by adding 1 to the type byte.
    resp[4] = (u8)(req[4] + 1);
    return (int)req_len;
}

// A responder that ALWAYS rejects with -1, simulating a backend error.
static int rejector_responder(void *ctx, const u8 *req, size_t req_len,
                                u8 *resp, size_t resp_cap) {
    (void)ctx; (void)req; (void)req_len; (void)resp; (void)resp_cap;
    return -1;
}

// Session-aware responder: matches Tversion → Rversion (NOTAG, msize
// echoed) and Tattach → Rattach (request's tag, qid populated). Used
// by the exchange-handshake test.
static int session_handshake_responder(void *ctx, const u8 *req, size_t req_len,
                                         u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if (type == P9_TVERSION) {
        // Rversion: msize(4) + version-string("9P2000.L", 2+8=10 bytes).
        size_t body_len = 4 + 2 + 8;
        size_t total = P9_HDR_LEN + body_len;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff);
        resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RVERSION;
        resp[5] = 0xff; resp[6] = 0xff;       // NOTAG echo
        // msize = 8192 (LE):
        resp[7] = 0x00; resp[8] = 0x20; resp[9] = 0x00; resp[10] = 0x00;
        resp[11] = 8; resp[12] = 0;
        const char *v = "9P2000.L";
        for (int i = 0; i < 8; i++) resp[13 + i] = (u8)v[i];
        return (int)total;
    } else if (type == P9_TATTACH) {
        // Rattach: qid(13) — type=P9_QTDIR, version=1, path=42.
        size_t total = P9_HDR_LEN + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff);
        resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RATTACH;
        resp[5] = (u8)(tag & 0xff);
        resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTDIR;                   // qid.type
        resp[8] = 1; resp[9] = 0; resp[10] = 0; resp[11] = 0;  // qid.version
        // qid.path = 42 (LE u64)
        resp[12] = 42; resp[13] = 0; resp[14] = 0; resp[15] = 0;
        resp[16] = 0; resp[17] = 0; resp[18] = 0; resp[19] = 0;
        return (int)total;
    } else if (type == P9_TWALK) {
        // Rwalk: nwqid(2) + 1 qid(13).
        size_t total = P9_HDR_LEN + 2 + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff);
        resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWALK;
        resp[5] = (u8)(tag & 0xff);
        resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = 1; resp[8] = 0;              // nwqid = 1
        resp[9] = P9_QTFILE;
        resp[10] = 0; resp[11] = 0; resp[12] = 0; resp[13] = 0;   // version
        resp[14] = 99; resp[15] = 0; resp[16] = 0; resp[17] = 0;  // path low
        resp[18] = 0;  resp[19] = 0; resp[20] = 0; resp[21] = 0;  // path high
        return (int)total;
    }
    return -1;
}

// =============================================================================
// Tests.
// =============================================================================

void test_9p_transport_init_destroy(void) {
    struct p9_loopback lb;
    TEST_EXPECT_EQ(p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL), 0,
                    "loopback init ok");

    struct p9_transport t;
    int rc = p9_transport_init(&t, p9_loopback_ops_for(&lb),
                                 g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "transport init ok");
    TEST_ASSERT(p9_transport_is_open(&t), "transport state = OPEN");

    p9_transport_destroy(&t);
    TEST_EXPECT_EQ((u32)t.magic, (u32)0, "destroy clobbers magic");
    TEST_ASSERT(!p9_transport_is_open(&t), "destroyed transport not open");

    // Invalid args refused.
    rc = p9_transport_init(NULL, p9_loopback_ops_for(&lb),
                             g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, -1, "init NULL transport refused");
    rc = p9_transport_init(&t, p9_loopback_ops_for(&lb), NULL, 1024);
    TEST_EXPECT_EQ(rc, -1, "init NULL recv_buf refused");
    rc = p9_transport_init(&t, p9_loopback_ops_for(&lb), g_recv_buf, 3);
    TEST_EXPECT_EQ(rc, -1, "init too-small recv_buf refused");

    p9_loopback_destroy(&lb);
}

void test_9p_transport_round_trip(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    // Build a Tclunk; the echo responder will flip type → P9_RCLUNK.
    int total = p9_build_tclunk(g_send_buf, sizeof(g_send_buf), 0x0042, 7);
    TEST_EXPECT_EQ(total, 11, "Tclunk built");

    int rc = p9_transport_round_trip(&t, g_send_buf, (size_t)total);
    TEST_EXPECT_EQ(rc, 11, "round_trip returns response length");
    TEST_EXPECT_EQ((u64)p9_transport_last_recv_len(&t), (u64)11,
                    "last_recv_len cached");
    // Verify the response header.
    u32 sz; u8 ty; u16 tg;
    p9_peek_header(g_recv_buf, 11, &sz, &ty, &tg);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_RCLUNK, "response type = RCLUNK");
    TEST_EXPECT_EQ((u64)tg, (u64)0x0042,    "response tag round-trip");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_send_frame_size_mismatch_rejected(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    // Build a valid Tclunk (size = 11) but pass len = 12 to send.
    int total = p9_build_tclunk(g_send_buf, sizeof(g_send_buf), 1, 0);
    TEST_EXPECT_EQ(total, 11, "Tclunk built");
    int rc = p9_transport_send(&t, g_send_buf, 12);
    TEST_EXPECT_EQ(rc, -1, "send with len != header.size rejected");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_recv_frame_too_large_rejected(void) {
    // Hand-craft a backend that returns a frame larger than recv_cap.
    // We do this by setting recv_cap small (32 bytes) and having the
    // responder produce a 64-byte response. Should fail on the body
    // length check.
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL);
    struct p9_transport t;
    u8 small_recv_buf[32];
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       small_recv_buf, sizeof(small_recv_buf));

    // Build a Twrite with 40 bytes payload (frame = 7+16+40 = 63 > 32).
    u8 payload[40];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (u8)i;
    int total = p9_build_twrite(g_send_buf, sizeof(g_send_buf), 1, 0,
                                  /*offset*/ 0, sizeof(payload), payload);
    TEST_EXPECT_EQ(total, 63, "Twrite built");
    int rc = p9_transport_round_trip(&t, g_send_buf, (size_t)total);
    TEST_EXPECT_EQ(rc, -1, "recv with frame > recv_cap rejected");
    TEST_ASSERT(!p9_transport_is_open(&t),
                "transport transitioned to ERROR on oversized frame");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_partial_read_aggregation(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL);
    // Force the loopback to dribble out 3 bytes per recv call. The
    // transport core must aggregate header + body until the full frame
    // is in hand.
    p9_loopback_set_chunk_size(&lb, 3);

    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    // Build a Tattach (28 bytes including header). Should require
    // ceil(28 / 3) = 10 ops.recv calls to aggregate.
    const u8 uname[] = {'r', 'o', 'o', 't'};
    const u8 aname[] = {'/'};
    int total = p9_build_tattach(g_send_buf, sizeof(g_send_buf), 0x55,
                                  /*fid*/ 0, P9_NOFID,
                                  uname, sizeof(uname),
                                  aname, sizeof(aname), 0);
    TEST_EXPECT_EQ(total, 28, "Tattach built");
    int rc = p9_transport_round_trip(&t, g_send_buf, (size_t)total);
    TEST_EXPECT_EQ(rc, 28, "round_trip succeeds despite 3-byte chunks");
    // Verify the response type was flipped to P9_RATTACH.
    u32 sz; u8 ty; u16 tg;
    p9_peek_header(g_recv_buf, 28, &sz, &ty, &tg);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_RATTACH, "type flipped through partial reads");
    TEST_EXPECT_EQ((u64)tg, (u64)0x55,       "tag preserved through partial reads");
    // Confirm the loopback's recvs counter > 1 (partial reads actually fired).
    TEST_ASSERT(lb.recvs > 1, "multiple ops.recv calls aggregated");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_backend_error_transitions_to_error(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), rejector_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    // Build a Tclunk; responder will reject with -1.
    int total = p9_build_tclunk(g_send_buf, sizeof(g_send_buf), 1, 0);
    int rc = p9_transport_send(&t, g_send_buf, (size_t)total);
    TEST_EXPECT_EQ(rc, -1, "send fails when backend rejects");
    TEST_ASSERT(!p9_transport_is_open(&t),
                "transport transitioned to ERROR on backend failure");

    // Further sends refused (state != OPEN).
    rc = p9_transport_send(&t, g_send_buf, (size_t)total);
    TEST_EXPECT_EQ(rc, -1, "post-error send refused");
    rc = p9_transport_recv(&t);
    TEST_EXPECT_EQ(rc, -1, "post-error recv refused");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_close_idempotent(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), echo_flip_type_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    int rc = p9_transport_close(&t);
    TEST_EXPECT_EQ(rc, 0, "first close ok");
    TEST_ASSERT(!p9_transport_is_open(&t), "post-close not open");

    rc = p9_transport_close(&t);
    TEST_EXPECT_EQ(rc, 0, "second close is idempotent (returns 0)");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_exchange_drives_session_handshake(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), session_handshake_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    struct p9_session s;
    p9_session_init(&s, /*root_fid=*/0, 8192);

    // Phase 1: Tversion → Rversion via exchange.
    int len = p9_session_send_version(&s, g_send_buf, sizeof(g_send_buf), NULL, 0);
    TEST_ASSERT(len > 0, "send_version ok");
    struct p9_dispatch_result r1;
    int rc = p9_transport_exchange(&t, &s, g_send_buf, (size_t)len, &r1);
    TEST_EXPECT_EQ(rc, 0,                       "exchange Rversion ok");
    TEST_EXPECT_EQ((u64)s.state, (u64)P9_SESS_VERSIONED, "state = VERSIONED");
    TEST_EXPECT_EQ((u64)r1.kind, (u64)P9_TVERSION, "result kind = TVERSION");
    TEST_EXPECT_EQ((u64)s.negotiated_msize, (u64)8192, "msize negotiated");

    // Phase 2: Tattach → Rattach via exchange.
    const u8 uname[] = {'r', 'o', 'o', 't'};
    const u8 aname[] = {'/'};
    len = p9_session_send_attach(&s, g_send_buf, sizeof(g_send_buf),
                                  uname, sizeof(uname),
                                  aname, sizeof(aname), 0);
    TEST_ASSERT(len > 0, "send_attach ok");
    struct p9_dispatch_result r2;
    rc = p9_transport_exchange(&t, &s, g_send_buf, (size_t)len, &r2);
    TEST_EXPECT_EQ(rc, 0,                       "exchange Rattach ok");
    TEST_EXPECT_EQ((u64)s.state, (u64)P9_SESS_OPEN, "state = OPEN");
    TEST_ASSERT(p9_session_fid_bound(&s, 0),    "root_fid bound after Rattach");
    TEST_EXPECT_EQ(r2.attach_qid.path, (u64)42, "Rattach qid.path round-trip");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}

void test_9p_transport_exchange_drives_session_walk(void) {
    struct p9_loopback lb;
    p9_loopback_init(&lb, g_loopback_resp_buf, sizeof(g_loopback_resp_buf), session_handshake_responder, NULL);
    struct p9_transport t;
    p9_transport_init(&t, p9_loopback_ops_for(&lb),
                       g_recv_buf, sizeof(g_recv_buf));

    struct p9_session s;
    p9_session_init(&s, /*root_fid=*/0, 8192);

    // Drive handshake.
    int len = p9_session_send_version(&s, g_send_buf, sizeof(g_send_buf), NULL, 0);
    struct p9_dispatch_result r;
    p9_transport_exchange(&t, &s, g_send_buf, (size_t)len, &r);
    const u8 uname[] = {'r', 'o', 'o', 't'};
    const u8 aname[] = {'/'};
    len = p9_session_send_attach(&s, g_send_buf, sizeof(g_send_buf),
                                  uname, sizeof(uname),
                                  aname, sizeof(aname), 0);
    p9_transport_exchange(&t, &s, g_send_buf, (size_t)len, &r);

    // Now walk root → fid 7 (clone).
    len = p9_session_send_walk(&s, g_send_buf, sizeof(g_send_buf),
                                /*src=*/0, /*new=*/7,
                                /*nwname=*/0, NULL, NULL);
    TEST_ASSERT(len > 0, "send_walk ok");
    struct p9_dispatch_result rw;
    int rc = p9_transport_exchange(&t, &s, g_send_buf, (size_t)len, &rw);
    TEST_EXPECT_EQ(rc, 0,                          "exchange Rwalk ok");
    TEST_ASSERT(p9_session_fid_bound(&s, 7),       "new_fid bound after Rwalk");
    TEST_EXPECT_EQ((u64)rw.kind, (u64)P9_TWALK,    "result kind = TWALK");
    TEST_EXPECT_EQ((u64)rw.nwqid, (u64)1,          "nwqid = 1");
    TEST_EXPECT_EQ(rw.qids[0].path, (u64)99,       "walked qid.path = 99");

    p9_transport_destroy(&t);
    p9_loopback_destroy(&lb);
}
