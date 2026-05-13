// 9P session state-machine unit tests (P5-session).
//
// Each test maps to a spec-level property from `specs/9p_client.tla`.
// The spec's 4 buggy cfg variants directly correspond to the
// rejection-path tests below: any impl that introduces a tag-collision,
// fid-after-clunk, OOO-mismatch, or unbounded-outstanding bug breaks
// at least one of these.
//
// State-machine coverage:
//   - INIT → VERSIONED → OPEN → CLOSED transitions.
//   - send_X gated on the matching state.
//   - dispatch_rmsg validates type matches outstanding's kind.

#include "test.h"

#include <thylacine/9p_session.h>
#include <thylacine/9p_wire.h>
#include <thylacine/types.h>

void test_9p_session_init_destroy(void);
void test_9p_session_version_handshake(void);
void test_9p_session_attach_handshake(void);
void test_9p_session_walk_round_trip(void);
void test_9p_session_clunk_round_trip(void);
void test_9p_session_clunk_send_time_unbinds(void);
void test_9p_session_dispatch_rlerror(void);
void test_9p_session_walk_to_root_refused(void);
void test_9p_session_walk_to_bound_fid_refused(void);
void test_9p_session_walk_from_unbound_fid_refused(void);
void test_9p_session_clunk_root_refused(void);
void test_9p_session_clunk_with_inflight_on_fid_refused(void);
void test_9p_session_fid_after_clunk_refused(void);
void test_9p_session_dispatch_wrong_tag_rejected(void);
void test_9p_session_dispatch_wrong_type_rejected(void);
void test_9p_session_close_with_inflight_refused(void);
void test_9p_session_state_gate_send_walk_before_open(void);

// 4 KiB scratch buffer.
static u8 g_buf[4096];

// Build an Rmsg of `type` with the given tag + body bytes. Returns the
// total length (header + body). Used to synthesize server replies for
// dispatch tests.
static int synth_rmsg(u8 *out, size_t cap, u8 type, u16 tag,
                      const u8 *body, size_t body_len) {
    size_t total = P9_HDR_LEN + body_len;
    if (total > cap) return -1;
    out[0] = (u8)(total       & 0xff);
    out[1] = (u8)((total >> 8) & 0xff);
    out[2] = (u8)((total >> 16) & 0xff);
    out[3] = (u8)((total >> 24) & 0xff);
    out[4] = type;
    out[5] = (u8)(tag       & 0xff);
    out[6] = (u8)((tag >> 8) & 0xff);
    for (size_t i = 0; i < body_len; i++) out[P9_HDR_LEN + i] = body[i];
    return (int)total;
}

// Synthesize an Rversion with the same tag (NOTAG), msize, and version
// the client sent. Returns rmsg length.
static int synth_rversion(u8 *out, size_t cap, u32 msize) {
    // Body: msize (u32 LE) + version string ("9P2000.L", len=8).
    u8 body[4 + 2 + 8];
    body[0] = (u8)(msize       & 0xff);
    body[1] = (u8)((msize >> 8) & 0xff);
    body[2] = (u8)((msize >> 16) & 0xff);
    body[3] = (u8)((msize >> 24) & 0xff);
    body[4] = 8; body[5] = 0;
    const char *v = "9P2000.L";
    for (int i = 0; i < 8; i++) body[6 + i] = (u8)v[i];
    return synth_rmsg(out, cap, P9_RVERSION, P9_NOTAG, body, sizeof(body));
}

// Synthesize an Rattach with the given qid + tag.
static int synth_rattach(u8 *out, size_t cap, u16 tag,
                          u8 qtype, u32 qver, u64 qpath) {
    u8 body[P9_QID_LEN];
    body[0] = qtype;
    for (int i = 0; i < 4; i++) body[1 + i] = (u8)((qver >> (i * 8)) & 0xff);
    for (int i = 0; i < 8; i++) body[5 + i] = (u8)((qpath >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, P9_RATTACH, tag, body, sizeof(body));
}

// Synthesize an Rwalk with a single qid (sufficient for tests).
static int synth_rwalk_single(u8 *out, size_t cap, u16 tag,
                               u8 qtype, u32 qver, u64 qpath) {
    u8 body[2 + P9_QID_LEN];
    body[0] = 1; body[1] = 0;        // nwqid = 1
    body[2] = qtype;
    for (int i = 0; i < 4; i++) body[3 + i] = (u8)((qver >> (i * 8)) & 0xff);
    for (int i = 0; i < 8; i++) body[7 + i] = (u8)((qpath >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, P9_RWALK, tag, body, sizeof(body));
}

// Synthesize an Rclunk (empty body).
static int synth_rclunk(u8 *out, size_t cap, u16 tag) {
    return synth_rmsg(out, cap, P9_RCLUNK, tag, NULL, 0);
}

// Synthesize an Rlerror with the given ecode.
static int synth_rlerror(u8 *out, size_t cap, u16 tag, u32 ecode) {
    u8 body[4];
    for (int i = 0; i < 4; i++) body[i] = (u8)((ecode >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, P9_RLERROR, tag, body, sizeof(body));
}

// Drive a session through INIT → VERSIONED → OPEN. Sets `s` open with
// root_fid bound. Returns 0 on success. Used by tests that need a
// fully-open session.
static int drive_session_open(struct p9_session *s, u32 root_fid) {
    int rc = p9_session_init(s, root_fid, 8192);
    if (rc < 0) return -1;
    // Tversion → Rversion.
    int len = p9_session_send_version(s, g_buf, sizeof(g_buf), NULL, 0);
    if (len < 0) return -1;
    len = synth_rversion(g_buf, sizeof(g_buf), 8192);
    if (len < 0) return -1;
    struct p9_dispatch_result r;
    rc = p9_session_dispatch_rmsg(s, g_buf, (size_t)len, &r);
    if (rc < 0) return -1;
    // Tattach → Rattach.
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    len = p9_session_send_attach(s, g_buf, sizeof(g_buf),
                                 uname, sizeof(uname),
                                 aname, sizeof(aname), 0);
    if (len < 0) return -1;
    // Read back the tag the impl allocated.
    u32 sz; u8 ty; u16 tag;
    rc = p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    if (rc < 0) return -1;
    len = synth_rattach(g_buf, sizeof(g_buf), tag, P9_QTDIR, 1, 100);
    if (len < 0) return -1;
    rc = p9_session_dispatch_rmsg(s, g_buf, (size_t)len, &r);
    if (rc < 0) return -1;
    return 0;
}

// =============================================================================
// Lifecycle.
// =============================================================================

void test_9p_session_init_destroy(void) {
    struct p9_session s;
    TEST_EXPECT_EQ(p9_session_init(&s, 0, 8192), 0, "init ok");
    TEST_ASSERT(p9_session_fid_bound(&s, 0) == false, "no fids bound at init");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "no inflight at init");

    p9_session_destroy(&s);
    TEST_EXPECT_EQ((u32)s.magic, (u32)0, "destroy clobbers magic");
    // Subsequent calls are no-ops on a destroyed session.
    TEST_ASSERT(p9_session_fid_bound(&s, 0) == false, "fid_bound on destroyed = false");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "inflight on destroyed = 0");

    // init refuses invalid args.
    TEST_EXPECT_EQ(p9_session_init(NULL, 0, 8192), -1, "init NULL refused");
    TEST_EXPECT_EQ(p9_session_init(&s, P9_NOFID, 8192), -1, "init NOFID refused");
    TEST_EXPECT_EQ(p9_session_init(&s, 0, 0), -1, "init msize=0 refused");
}

// =============================================================================
// Handshake.
// =============================================================================

void test_9p_session_version_handshake(void) {
    struct p9_session s;
    p9_session_init(&s, /*root_fid=*/0, 16384);

    // Pre-version: send_attach refused.
    int rc = p9_session_send_attach(&s, g_buf, sizeof(g_buf),
                                    (const u8 *)"r", 1, (const u8 *)"/", 1, 0);
    TEST_EXPECT_EQ(rc, -1, "send_attach before Rversion refused");

    // Send Tversion.
    int len = p9_session_send_version(&s, g_buf, sizeof(g_buf), NULL, 0);
    TEST_ASSERT(len > 0, "send_version ok");

    // Synthesize Rversion with msize=8192 (smaller than client's 16384).
    len = synth_rversion(g_buf, sizeof(g_buf), 8192);
    TEST_ASSERT(len > 0, "synth Rversion");
    struct p9_dispatch_result r;
    rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rversion ok");
    TEST_EXPECT_EQ((u64)s.state, (u64)P9_SESS_VERSIONED, "post-Rversion state");
    TEST_EXPECT_EQ((u64)s.negotiated_msize, (u64)8192, "negotiated msize = server's smaller value");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TVERSION, "result kind = TVERSION");
    TEST_EXPECT_EQ((u64)r.version_len, (u64)8, "version string len");
}

void test_9p_session_attach_handshake(void) {
    struct p9_session s;
    p9_session_init(&s, /*root_fid=*/0, 8192);

    // Drive Tversion + Rversion.
    int len = p9_session_send_version(&s, g_buf, sizeof(g_buf), NULL, 0);
    TEST_ASSERT(len > 0, "Tversion built");
    len = synth_rversion(g_buf, sizeof(g_buf), 8192);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);

    // Tattach.
    len = p9_session_send_attach(&s, g_buf, sizeof(g_buf),
                                 (const u8 *)"alice", 5,
                                 (const u8 *)"/", 1, 1000);
    TEST_ASSERT(len > 0, "send_attach ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TATTACH, "Tattach type");
    // Tag should be 0 (first allocation).
    TEST_EXPECT_EQ((u64)tag, (u64)0, "first allocated tag = 0");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)1, "one inflight after Tattach");

    // Synthesize Rattach.
    len = synth_rattach(g_buf, sizeof(g_buf), tag, P9_QTDIR, 1, 42);
    struct p9_dispatch_result r2;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r2);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rattach ok");
    TEST_EXPECT_EQ((u64)s.state, (u64)P9_SESS_OPEN, "post-Rattach state = OPEN");
    TEST_ASSERT(p9_session_fid_bound(&s, 0), "root_fid bound after Rattach");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "no inflight after dispatch");
    TEST_EXPECT_EQ((u64)r2.kind, (u64)P9_TATTACH, "result kind = TATTACH");
    TEST_EXPECT_EQ(r2.attach_qid.path, (u64)42, "Rattach qid path round-trip");
}

// =============================================================================
// Walk + clunk.
// =============================================================================

void test_9p_session_walk_round_trip(void) {
    struct p9_session s;
    TEST_EXPECT_EQ(drive_session_open(&s, /*root_fid=*/0), 0, "drive_session_open");
    TEST_ASSERT(p9_session_is_open(&s), "session OPEN");

    // Walk root → fid 5 (clone, no names).
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf),
                                    /*src=*/0, /*new=*/5,
                                    /*nwname=*/0, NULL, NULL);
    TEST_ASSERT(len > 0, "send_walk(0→5, clone) ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TWALK, "Twalk type");

    // Before dispatch: new_fid NOT yet bound.
    TEST_ASSERT(!p9_session_fid_bound(&s, 5), "new_fid not bound pre-dispatch");

    // Synthesize Rwalk with single qid.
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTDIR, 1, 200);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rwalk ok");
    TEST_ASSERT(p9_session_fid_bound(&s, 5), "new_fid bound after Rwalk");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TWALK, "result kind = TWALK");
    TEST_EXPECT_EQ((u64)r.nwqid, (u64)1, "Rwalk nwqid round-trip");
    TEST_EXPECT_EQ(r.qids[0].path, (u64)200, "Rwalk qid[0].path round-trip");
}

void test_9p_session_clunk_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, /*root_fid=*/0);
    // Walk + bind fid 7.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 7, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 1, 300);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_ASSERT(p9_session_fid_bound(&s, 7), "fid 7 bound after walk");

    // Clunk fid 7.
    len = p9_session_send_clunk(&s, g_buf, sizeof(g_buf), 7);
    TEST_ASSERT(len > 0, "send_clunk(7) ok");
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TCLUNK, "Tclunk type");

    // Synthesize Rclunk.
    len = synth_rclunk(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r2;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r2);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rclunk ok");
    TEST_EXPECT_EQ((u64)r2.kind, (u64)P9_TCLUNK, "result kind = TCLUNK");
    TEST_ASSERT(!p9_session_fid_bound(&s, 7), "fid 7 unbound after Rclunk");
}

// Verifies Send-time unbind discipline: as soon as Tclunk is sent, fid
// is unbound — subsequent send_walk targeting it as src fails.
void test_9p_session_clunk_send_time_unbinds(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 9, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 1, 0);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);

    // Send_clunk on 9 — Send-time unbinds.
    len = p9_session_send_clunk(&s, g_buf, sizeof(g_buf), 9);
    TEST_ASSERT(len > 0, "send_clunk(9) accepted");
    TEST_ASSERT(!p9_session_fid_bound(&s, 9), "fid 9 unbound at Send-time of Tclunk");

    // Now attempt send_walk from 9 (which is no longer bound). Should fail.
    int len2 = p9_session_send_walk(&s, g_buf + len, sizeof(g_buf) - len,
                                     9, 11, 0, NULL, NULL);
    TEST_EXPECT_EQ(len2, -1, "send_walk from unbound (post Tclunk) refused");
}

// =============================================================================
// Rlerror surfaces ecode; does not mutate fid state (except clunk's
// Send-time unbind which already happened).
// =============================================================================

void test_9p_session_dispatch_rlerror(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Walk for fid 3.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 3, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);

    // Server replies Rlerror (ENOENT=2).
    len = synth_rlerror(g_buf, sizeof(g_buf), tag, 2);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rlerror ok");
    TEST_ASSERT(r.is_error, "result.is_error = true");
    TEST_EXPECT_EQ((u64)r.ecode, (u64)2, "Rlerror ecode = ENOENT");
    // fid 3 should NOT be bound (walk failed).
    TEST_ASSERT(!p9_session_fid_bound(&s, 3), "fid not bound on Rlerror for Twalk");
    // Outstanding cleared.
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "inflight cleared after Rlerror");
}

// =============================================================================
// Send-time precondition rejection paths (spec's buggy-cfg shapes).
// =============================================================================

void test_9p_session_walk_to_root_refused(void) {
    struct p9_session s;
    drive_session_open(&s, /*root_fid=*/0);
    int rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, /*new=*/0, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "walk targeting root fid refused");
}

void test_9p_session_walk_to_bound_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Walk + bind fid 4.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 4, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 1, 0);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_ASSERT(p9_session_fid_bound(&s, 4), "fid 4 bound");
    // Walk targeting already-bound fid refused.
    int rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, /*new=*/4, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "walk to already-bound fid refused");
}

void test_9p_session_walk_from_unbound_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    int rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf),
                                   /*src=*/99 /*not bound*/, /*new=*/2, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "walk from unbound src refused");
}

void test_9p_session_clunk_root_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    int rc = p9_session_send_clunk(&s, g_buf, sizeof(g_buf), 0 /*root*/);
    TEST_EXPECT_EQ(rc, -1, "clunk on root refused");
}

void test_9p_session_clunk_with_inflight_on_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Walk → bind fid 6.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 6, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 1, 0);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    // Issue another walk that uses fid 6 as src — leaves an in-flight op on 6.
    len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 6, 12, 0, NULL, NULL);
    TEST_ASSERT(len > 0, "second walk from 6 ok");
    // Now attempt clunk on 6 — refused because of in-flight op on 6.
    int rc = p9_session_send_clunk(&s, g_buf, sizeof(g_buf), 6);
    TEST_EXPECT_EQ(rc, -1, "clunk with in-flight op on same fid refused");
}

// Equivalent of spec's BuggyFidAfterClunkSend: after a successful Tclunk,
// subsequent ops on that fid are refused.
void test_9p_session_fid_after_clunk_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Walk + bind fid 8.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 8, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 1, 0);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    // Clunk + dispatch Rclunk → fid fully released.
    len = p9_session_send_clunk(&s, g_buf, sizeof(g_buf), 8);
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rclunk(g_buf, sizeof(g_buf), tag);
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_ASSERT(!p9_session_fid_bound(&s, 8), "fid 8 fully clunked");
    // Subsequent walk from 8 → refused.
    int rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 8, 10, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "send_walk from post-clunk fid refused (I-11 fid-after-clunk)");
}

// =============================================================================
// Dispatch rejection paths.
// =============================================================================

void test_9p_session_dispatch_wrong_tag_rejected(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // No outstanding ops; synthesize an Rwalk with arbitrary tag.
    int len = synth_rwalk_single(g_buf, sizeof(g_buf), 3, P9_QTFILE, 1, 0);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, -1, "dispatch with unmatched tag rejected");
}

void test_9p_session_dispatch_wrong_type_rejected(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Send Twalk; dispatch a synthesized Rclunk under the SAME tag.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 2, 0, NULL, NULL);
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    len = synth_rclunk(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, -1, "dispatch Rclunk against outstanding Twalk rejected");
}

// =============================================================================
// State-machine gating.
// =============================================================================

void test_9p_session_close_with_inflight_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Issue a walk to get an inflight op.
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 13, 0, NULL, NULL);
    TEST_ASSERT(len > 0, "walk ok");
    int rc = p9_session_close(&s);
    TEST_EXPECT_EQ(rc, -1, "close with inflight refused");
}

void test_9p_session_state_gate_send_walk_before_open(void) {
    struct p9_session s;
    p9_session_init(&s, 0, 8192);
    // In INIT state: send_walk refused.
    int rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 1, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "send_walk in INIT state refused");
    // Drive to VERSIONED.
    int len = p9_session_send_version(&s, g_buf, sizeof(g_buf), NULL, 0);
    (void)len;
    len = synth_rversion(g_buf, sizeof(g_buf), 8192);
    struct p9_dispatch_result r;
    p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    // Still refused in VERSIONED.
    rc = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 1, 0, NULL, NULL);
    TEST_EXPECT_EQ(rc, -1, "send_walk in VERSIONED state refused");
}
