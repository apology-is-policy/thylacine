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
void test_9p_session_lopen_round_trip(void);
void test_9p_session_lcreate_round_trip(void);
void test_9p_session_read_round_trip(void);
void test_9p_session_write_round_trip(void);
void test_9p_session_lopen_with_inflight_on_fid_refused(void);
void test_9p_session_read_permits_concurrent(void);
void test_9p_session_io_from_unbound_fid_refused(void);
void test_9p_session_io_before_open_refused(void);
void test_9p_session_getattr_round_trip(void);
void test_9p_session_setattr_round_trip(void);
void test_9p_session_readdir_round_trip(void);
void test_9p_session_statfs_round_trip(void);
void test_9p_session_weft_round_trip(void);
void test_9p_session_fsync_round_trip(void);
void test_9p_session_setattr_with_inflight_on_fid_refused(void);
void test_9p_session_getattr_permits_concurrent(void);
void test_9p_session_meta_from_unbound_fid_refused(void);
void test_9p_session_symlink_round_trip(void);
void test_9p_session_mknod_round_trip(void);
void test_9p_session_rename_round_trip(void);
void test_9p_session_readlink_round_trip(void);
void test_9p_session_link_round_trip(void);
void test_9p_session_mkdir_round_trip(void);
void test_9p_session_renameat_round_trip(void);
void test_9p_session_unlinkat_round_trip(void);
void test_9p_session_rename_with_inflight_on_fid_refused(void);
void test_9p_session_unlinkat_permits_concurrent(void);
void test_9p_session_mutation_from_unbound_fid_refused(void);
void test_9p_session_flush_reclaims_both(void);
void test_9p_session_late_reply_does_not_free_awaiting_flush(void);

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

// RW-4 round-2 R-B-F1: a LOCAL fid-table-full condition must NOT latch the shared
// session dead. fid_bind fails on capacity (n_bound_fids >= 256), which is not a
// server protocol fault -- R3-F1's mark_dead-on-drc<0 would otherwise take the
// whole root FS down on the 257th concurrent fid. Two legs: send_walk fails CLOSED
// when the table is full (the primary defense), and a dispatch-time fid_bind
// failure (the TOCTOU residual) surfaces as a per-op error (rc==0, is_error),
// NOT the -1 the client reads as a protocol violation.
void test_9p_session_walk_fid_full_no_latch(void) {
    struct p9_session s;
    TEST_EXPECT_EQ(drive_session_open(&s, /*root_fid=*/0), 0, "drive_session_open");

    // (a) SEND-time capacity pre-check: a full fid table fails the walk CLOSED
    // here (clean -1, no server round-trip), not at dispatch.
    size_t saved = s.n_bound_fids;
    s.n_bound_fids = P9_SESSION_MAX_FIDS;
    int len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 5, 0, NULL, NULL);
    TEST_ASSERT(len < 0, "send_walk fails closed when the fid table is full");
    s.n_bound_fids = saved;

    // (b) DISPATCH-time TOCTOU: send a walk (table not yet full), then fill the
    // table (modeling a peer binding the last fid during this op's recv), then
    // dispatch the conformant Rwalk. Pre-fix this returned -1 -> the client
    // latched the shared session dead; post-fix it returns 0 + is_error so the
    // op fails with -EIO and the session stays ALIVE.
    len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), 0, 6, 0, NULL, NULL);
    TEST_ASSERT(len > 0, "send_walk(0->6) ok (table not yet full)");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    s.n_bound_fids = P9_SESSION_MAX_FIDS;   // the table fills during the recv window
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTDIR, 1, 200);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch returns 0 (per-op error), NOT -1 (would latch the session dead)");
    TEST_ASSERT(r.is_error, "local fid-full surfaces as a per-op error, not a protocol violation");
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

// =============================================================================
// IO family (P5-wire-io): end-to-end tests for Tlopen, Tlcreate, Tread,
// Twrite + their R-messages. Each drives a session to OPEN, walks a fid,
// then exercises the IO op + dispatch.
// =============================================================================

// Synthesize Rlopen / Rlcreate (identical body shape: qid + iounit).
static int synth_ropen_create(u8 *out, size_t cap, u8 type, u16 tag,
                                u8 qtype, u32 qver, u64 qpath, u32 iounit) {
    u8 body[P9_QID_LEN + 4];
    body[0] = qtype;
    for (int i = 0; i < 4; i++) body[1 + i] = (u8)((qver >> (i * 8)) & 0xff);
    for (int i = 0; i < 8; i++) body[5 + i] = (u8)((qpath >> (i * 8)) & 0xff);
    for (int i = 0; i < 4; i++) body[P9_QID_LEN + i] = (u8)((iounit >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, type, tag, body, sizeof(body));
}

// Synthesize Rread with the given payload bytes. count is encoded as the
// payload length.
static int synth_rread(u8 *out, size_t cap, u16 tag,
                        const u8 *payload, u32 payload_len) {
    u8 body[1024];
    if ((size_t)payload_len + 4 > sizeof(body)) return -1;
    for (int i = 0; i < 4; i++) body[i] = (u8)((payload_len >> (i * 8)) & 0xff);
    for (u32 i = 0; i < payload_len; i++) body[4 + i] = payload[i];
    return synth_rmsg(out, cap, P9_RREAD, tag, body, (size_t)(4 + payload_len));
}

// Synthesize Rwrite with the given accepted-count.
static int synth_rwrite(u8 *out, size_t cap, u16 tag, u32 count) {
    u8 body[4];
    for (int i = 0; i < 4; i++) body[i] = (u8)((count >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, P9_RWRITE, tag, body, sizeof(body));
}

// Walk root → new_fid + dispatch Rwalk, leaving new_fid bound + ready
// for IO. Returns 0 on success, -1 on failure.
static int walk_to(struct p9_session *s, u32 new_fid) {
    int len = p9_session_send_walk(s, g_buf, sizeof(g_buf),
                                    /*src=*/s->root_fid, new_fid,
                                    /*nwname=*/0, NULL, NULL);
    if (len < 0) return -1;
    u32 sz; u8 ty; u16 tag;
    if (p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag) < 0) return -1;
    len = synth_rwalk_single(g_buf, sizeof(g_buf), tag, P9_QTFILE, 0, 0);
    if (len < 0) return -1;
    struct p9_dispatch_result r;
    return p9_session_dispatch_rmsg(s, g_buf, (size_t)len, &r);
}

void test_9p_session_lopen_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, /*root_fid=*/0);
    TEST_EXPECT_EQ(walk_to(&s, 11), 0, "walk to fid 11");

    int len = p9_session_send_lopen(&s, g_buf, sizeof(g_buf), 11, /*flags=*/0);
    TEST_ASSERT(len > 0, "send_lopen ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TLOPEN, "Tlopen type");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)1, "lopen inflight = 1");

    // Synthesize Rlopen.
    len = synth_ropen_create(g_buf, sizeof(g_buf), P9_RLOPEN, tag,
                              P9_QTFILE, 5, 555ull, 4096u);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                            "dispatch Rlopen ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TLOPEN,      "result kind = TLOPEN");
    TEST_EXPECT_EQ(r.open_qid.path, (u64)555,        "Rlopen qid.path");
    TEST_EXPECT_EQ((u64)r.open_iounit, (u64)4096,    "Rlopen iounit");
    // Fid stays bound.
    TEST_ASSERT(p9_session_fid_bound(&s, 11),        "fid 11 still bound after Rlopen");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "no inflight post-dispatch");
}

void test_9p_session_lcreate_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 12), 0, "walk to fid 12 (parent dir)");

    const u8 name[] = {'n','e','w'};
    int len = p9_session_send_lcreate(&s, g_buf, sizeof(g_buf),
                                       12, name, sizeof(name),
                                       /*flags=*/0x42u, /*mode=*/0644u, /*gid=*/0);
    TEST_ASSERT(len > 0, "send_lcreate ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TLCREATE, "Tlcreate type");

    len = synth_ropen_create(g_buf, sizeof(g_buf), P9_RLCREATE, tag,
                              P9_QTFILE, 1, 777ull, 0);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                            "dispatch Rlcreate ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TLCREATE,    "result kind = TLCREATE");
    TEST_EXPECT_EQ(r.open_qid.path, (u64)777,        "Rlcreate qid.path");
    TEST_ASSERT(p9_session_fid_bound(&s, 12),        "fid 12 still bound (now to new file)");
}

void test_9p_session_read_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 13), 0, "walk to fid 13");

    int len = p9_session_send_read(&s, g_buf, sizeof(g_buf),
                                    13, /*offset=*/0, /*count=*/256);
    TEST_ASSERT(len > 0, "send_read ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TREAD, "Tread type");

    const u8 payload[] = {'h','i','!'};
    len = synth_rread(g_buf, sizeof(g_buf), tag, payload, (u32)sizeof(payload));
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rread ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TREAD,           "result kind = TREAD");
    TEST_EXPECT_EQ((u64)r.read_count, (u64)sizeof(payload), "Rread count");
    TEST_ASSERT(r.read_data != NULL,                     "Rread data non-null");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(r.read_data[i] == payload[i],        "Rread data byte mismatch");
    }
}

void test_9p_session_write_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 14), 0, "walk to fid 14");

    const u8 payload[] = {'b','y','t','e','s'};
    int len = p9_session_send_write(&s, g_buf, sizeof(g_buf),
                                     14, /*offset=*/0,
                                     (u32)sizeof(payload), payload);
    TEST_ASSERT(len > 0, "send_write ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TWRITE, "Twrite type");

    len = synth_rwrite(g_buf, sizeof(g_buf), tag, (u32)sizeof(payload));
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rwrite ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TWRITE,          "result kind = TWRITE");
    TEST_EXPECT_EQ((u64)r.write_count, (u64)sizeof(payload), "Rwrite count");
}

// Tlopen on a fid with another op in flight on it: refused.
void test_9p_session_lopen_with_inflight_on_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 15), 0, "walk to fid 15");

    // First lopen: accepted.
    int len = p9_session_send_lopen(&s, g_buf, sizeof(g_buf), 15, 0);
    TEST_ASSERT(len > 0, "first lopen accepted");

    // Second lopen on same fid while first is in flight: refused.
    int rc = p9_session_send_lopen(&s, g_buf, sizeof(g_buf), 15, 0);
    TEST_EXPECT_EQ(rc, -1, "concurrent lopen on same fid refused");
}

// Tread permits concurrent ops on the same fid (offset is explicit).
void test_9p_session_read_permits_concurrent(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 16), 0, "walk to fid 16");

    int len1 = p9_session_send_read(&s, g_buf, sizeof(g_buf),
                                     16, /*offset=*/0, /*count=*/256);
    TEST_ASSERT(len1 > 0, "first read accepted");

    // Need separate buffer for the second message (g_buf is shared).
    static u8 buf2[1024];
    int len2 = p9_session_send_read(&s, buf2, sizeof(buf2),
                                     16, /*offset=*/256, /*count=*/256);
    TEST_ASSERT(len2 > 0, "concurrent read on same fid accepted");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2, "two reads inflight");
}

void test_9p_session_io_from_unbound_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Fid 99 is never bound.
    int rc = p9_session_send_lopen(&s, g_buf, sizeof(g_buf), 99, 0);
    TEST_EXPECT_EQ(rc, -1, "lopen on unbound fid refused");
    rc = p9_session_send_read(&s, g_buf, sizeof(g_buf), 99, 0, 1);
    TEST_EXPECT_EQ(rc, -1, "read on unbound fid refused");
    rc = p9_session_send_write(&s, g_buf, sizeof(g_buf), 99, 0, 1, (const u8 *)"x");
    TEST_EXPECT_EQ(rc, -1, "write on unbound fid refused");
    const u8 name[] = {'x'};
    rc = p9_session_send_lcreate(&s, g_buf, sizeof(g_buf), 99,
                                  name, sizeof(name), 0, 0, 0);
    TEST_EXPECT_EQ(rc, -1, "lcreate on unbound fid refused");
}

void test_9p_session_io_before_open_refused(void) {
    struct p9_session s;
    p9_session_init(&s, /*root_fid=*/0, 8192);
    // In INIT state: every IO send refused.
    int rc = p9_session_send_lopen(&s, g_buf, sizeof(g_buf), 0, 0);
    TEST_EXPECT_EQ(rc, -1, "lopen in INIT state refused");
    rc = p9_session_send_read(&s, g_buf, sizeof(g_buf), 0, 0, 1);
    TEST_EXPECT_EQ(rc, -1, "read in INIT state refused");
    rc = p9_session_send_write(&s, g_buf, sizeof(g_buf), 0, 0, 1, (const u8 *)"x");
    TEST_EXPECT_EQ(rc, -1, "write in INIT state refused");
    const u8 name[] = {'x'};
    rc = p9_session_send_lcreate(&s, g_buf, sizeof(g_buf), 0,
                                  name, sizeof(name), 0, 0, 0);
    TEST_EXPECT_EQ(rc, -1, "lcreate in INIT state refused");
}

// =============================================================================
// Metadata family (P5-wire-meta): end-to-end tests for Tgetattr / Tsetattr
// / Treaddir / Tstatfs / Tfsync.
// =============================================================================

// Synthesize Rgetattr with a small but distinguishable attribute record.
// Body shape: 8 (valid) + 13 (qid) + 12 (3 u32) + 15*8 (15 u64) = 153 bytes.
static int synth_rgetattr(u8 *out, size_t cap, u16 tag) {
    u8 body[8 + P9_QID_LEN + 4 * 3 + 8 * 15];
    size_t off = 0;
    int rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, P9_GETATTR_BASIC); off += (size_t)rc;
    struct p9_qid q = { .type = P9_QTFILE, .version = 3, .path = 808 };
    rc = p9_pack_qid(body + off, sizeof(body) - off, &q);  off += (size_t)rc;
    rc = p9_pack_u32(body + off, sizeof(body) - off, 0644u); off += (size_t)rc;  // mode
    rc = p9_pack_u32(body + off, sizeof(body) - off, 0u);    off += (size_t)rc;  // uid
    rc = p9_pack_u32(body + off, sizeof(body) - off, 0u);    off += (size_t)rc;  // gid
    rc = p9_pack_u64(body + off, sizeof(body) - off, 1ull);  off += (size_t)rc;  // nlink
    rc = p9_pack_u64(body + off, sizeof(body) - off, 0ull);  off += (size_t)rc;  // rdev
    rc = p9_pack_u64(body + off, sizeof(body) - off, 999ull);off += (size_t)rc;  // size
    rc = p9_pack_u64(body + off, sizeof(body) - off, 4096ull);off += (size_t)rc; // blksize
    rc = p9_pack_u64(body + off, sizeof(body) - off, 2ull);  off += (size_t)rc;  // blocks
    for (int i = 0; i < 8; i++) {
        rc = p9_pack_u64(body + off, sizeof(body) - off, (u64)(0x200 + i));
        off += (size_t)rc;
    }
    rc = p9_pack_u64(body + off, sizeof(body) - off, 0ull);  off += (size_t)rc;  // gen
    rc = p9_pack_u64(body + off, sizeof(body) - off, 0ull);  off += (size_t)rc;  // data_version
    return synth_rmsg(out, cap, P9_RGETATTR, tag, body, off);
}

static int synth_rsetattr(u8 *out, size_t cap, u16 tag) {
    return synth_rmsg(out, cap, P9_RSETATTR, tag, NULL, 0);
}

static int synth_rreaddir(u8 *out, size_t cap, u16 tag,
                           const u8 *payload, u32 payload_len) {
    u8 body[256];
    if ((size_t)payload_len + 4 > sizeof(body)) return -1;
    for (int i = 0; i < 4; i++) body[i] = (u8)((payload_len >> (i * 8)) & 0xff);
    for (u32 i = 0; i < payload_len; i++) body[4 + i] = payload[i];
    return synth_rmsg(out, cap, P9_RREADDIR, tag, body, (size_t)(4 + payload_len));
}

static int synth_rstatfs(u8 *out, size_t cap, u16 tag) {
    u8 body[60];
    size_t off = 0;
    int rc;
    rc = p9_pack_u32(body + off, sizeof(body) - off, 0x53544D31u); off += (size_t)rc;
    rc = p9_pack_u32(body + off, sizeof(body) - off, 4096u);       off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 1000ull);     off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 500ull);      off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 250ull);      off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 64ull);       off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 16ull);       off += (size_t)rc;
    rc = p9_pack_u64(body + off, sizeof(body) - off, 0xCAFEull);   off += (size_t)rc;
    rc = p9_pack_u32(body + off, sizeof(body) - off, 255u);        off += (size_t)rc;
    return synth_rmsg(out, cap, P9_RSTATFS, tag, body, off);
}

static int synth_rfsync(u8 *out, size_t cap, u16 tag) {
    return synth_rmsg(out, cap, P9_RFSYNC, tag, NULL, 0);
}

void test_9p_session_getattr_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, /*root_fid=*/0);
    TEST_EXPECT_EQ(walk_to(&s, 30), 0, "walk to fid 30");

    int len = p9_session_send_getattr(&s, g_buf, sizeof(g_buf),
                                       30, P9_GETATTR_BASIC);
    TEST_ASSERT(len > 0, "send_getattr ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TGETATTR, "Tgetattr type");

    len = synth_rgetattr(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rgetattr ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TGETATTR,        "result kind = TGETATTR");
    TEST_EXPECT_EQ(r.attr.valid, (u64)P9_GETATTR_BASIC,  "Rgetattr valid round-trip");
    TEST_EXPECT_EQ(r.attr.qid.path, (u64)808,            "Rgetattr qid.path");
    TEST_EXPECT_EQ((u64)r.attr.mode, (u64)0644,          "Rgetattr mode");
    TEST_EXPECT_EQ(r.attr.size, (u64)999,                "Rgetattr size");
}

void test_9p_session_setattr_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 31), 0, "walk to fid 31");

    struct p9_setattr sa = {
        .valid = P9_SETATTR_MODE,
        .mode  = 0755u,
    };
    int len = p9_session_send_setattr(&s, g_buf, sizeof(g_buf), 31, &sa);
    TEST_ASSERT(len > 0, "send_setattr ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TSETATTR, "Tsetattr type");

    len = synth_rsetattr(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                          "dispatch Rsetattr ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TSETATTR,  "result kind = TSETATTR");
    TEST_ASSERT(p9_session_fid_bound(&s, 31),      "fid 31 still bound after Rsetattr");
}

void test_9p_session_readdir_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 32), 0, "walk to fid 32");

    int len = p9_session_send_readdir(&s, g_buf, sizeof(g_buf),
                                       32, /*offset*/ 0, /*count*/ 4096);
    TEST_ASSERT(len > 0, "send_readdir ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TREADDIR, "Treaddir type");

    // Dirent stream: one entry "x"
    u8 ent[64];
    size_t epos = 0;
    int rc;
    struct p9_qid q = { .type = P9_QTFILE, .version = 0, .path = 7 };
    rc = p9_pack_qid(ent + epos, sizeof(ent) - epos, &q);   epos += (size_t)rc;
    rc = p9_pack_u64(ent + epos, sizeof(ent) - epos, 1ull); epos += (size_t)rc;
    rc = p9_pack_u8 (ent + epos, sizeof(ent) - epos, 8u);   epos += (size_t)rc;  // DT_REG
    rc = p9_pack_str(ent + epos, sizeof(ent) - epos, (const u8 *)"x", 1);
    epos += (size_t)rc;

    len = synth_rreaddir(g_buf, sizeof(g_buf), tag, ent, (u32)epos);
    struct p9_dispatch_result r;
    rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                  "dispatch Rreaddir ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TREADDIR,          "result kind = TREADDIR");
    TEST_EXPECT_EQ((u64)r.readdir_count, (u64)epos,        "readdir count");
    TEST_ASSERT(r.readdir_data != NULL,                    "readdir data non-null");
    // Parse the first dirent.
    struct p9_qid dq = { 0 };
    u64 dent_off = 0;
    u8 dent_type = 0;
    const u8 *name_ptr = NULL;
    u16 name_len = 0;
    int dn = p9_unpack_dirent(r.readdir_data, (size_t)r.readdir_count,
                                &dq, &dent_off, &dent_type, &name_ptr, &name_len);
    TEST_EXPECT_EQ((u64)dn, (u64)epos,             "dirent total len");
    TEST_EXPECT_EQ(dq.path, (u64)7,                "dirent qid.path");
    TEST_EXPECT_EQ((u64)name_len, (u64)1,          "dirent name len");
    TEST_ASSERT(name_ptr != NULL && name_ptr[0] == 'x', "dirent name byte");
}

void test_9p_session_statfs_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 33), 0, "walk to fid 33");

    int len = p9_session_send_statfs(&s, g_buf, sizeof(g_buf), 33);
    TEST_ASSERT(len > 0, "send_statfs ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TSTATFS, "Tstatfs type");

    len = synth_rstatfs(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rstatfs ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TSTATFS,         "result kind = TSTATFS");
    TEST_EXPECT_EQ((u64)r.statfs.type, (u64)0x53544D31u, "statfs.type round-trip");
    TEST_EXPECT_EQ((u64)r.statfs.bsize, (u64)4096,       "statfs.bsize");
    TEST_EXPECT_EQ(r.statfs.blocks, (u64)1000,           "statfs.blocks");
    TEST_EXPECT_EQ(r.statfs.fsid, (u64)0xCAFE,           "statfs.fsid");
}

// Weft-6a-1: send_weft (read-shaped) -> dispatch Rweft -> the share_id +
// ring geometry surface in dispatch_result.weft_geom (by value, no rmsg alias).
void test_9p_session_weft_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 34), 0, "walk to fid 34");

    int len = p9_session_send_weft(&s, g_buf, sizeof(g_buf), 34);
    TEST_ASSERT(len > 0, "send_weft ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TWEFT, "Tweft type");

    struct p9_weft_geom geom = {
        .share_id     = 0xDEADBEEFCAFEF00DULL,
        .ring_size    = 0x00008000,   // 32 KiB
        .ring_entries = 128,
    };
    len = p9_build_rweft(g_buf, sizeof(g_buf), tag, &geom);
    TEST_ASSERT(len > 0, "build_rweft ok");
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                            "dispatch Rweft ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TWEFT,                       "result kind = TWEFT");
    TEST_EXPECT_EQ(r.weft_geom.share_id, (u64)0xDEADBEEFCAFEF00DULL, "weft_geom.share_id round-trip");
    TEST_EXPECT_EQ((u64)r.weft_geom.ring_size, (u64)0x00008000,      "weft_geom.ring_size");
    TEST_EXPECT_EQ((u64)r.weft_geom.ring_entries, (u64)128,          "weft_geom.ring_entries");
}

void test_9p_session_fsync_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 34), 0, "walk to fid 34");

    int len = p9_session_send_fsync(&s, g_buf, sizeof(g_buf),
                                     34, /*datasync*/ 1);
    TEST_ASSERT(len > 0, "send_fsync ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TFSYNC, "Tfsync type");

    len = synth_rfsync(g_buf, sizeof(g_buf), tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                       "dispatch Rfsync ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TFSYNC, "result kind = TFSYNC");
}

void test_9p_session_setattr_with_inflight_on_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 35), 0, "walk to fid 35");

    // First setattr in flight.
    struct p9_setattr sa = { .valid = P9_SETATTR_MODE, .mode = 0644u };
    int len = p9_session_send_setattr(&s, g_buf, sizeof(g_buf), 35, &sa);
    TEST_ASSERT(len > 0, "first setattr accepted");

    // Second setattr on same fid: refused.
    int rc = p9_session_send_setattr(&s, g_buf, sizeof(g_buf), 35, &sa);
    TEST_EXPECT_EQ(rc, -1, "concurrent setattr on same fid refused");
}

void test_9p_session_getattr_permits_concurrent(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 36), 0, "walk to fid 36");

    int len1 = p9_session_send_getattr(&s, g_buf, sizeof(g_buf),
                                        36, P9_GETATTR_BASIC);
    TEST_ASSERT(len1 > 0, "first getattr accepted");

    static u8 buf2[1024];
    int len2 = p9_session_send_getattr(&s, buf2, sizeof(buf2),
                                        36, P9_GETATTR_ALL);
    TEST_ASSERT(len2 > 0, "concurrent getattr on same fid accepted");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2, "two getattrs inflight");
}

void test_9p_session_meta_from_unbound_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Fid 88 never bound.
    int rc = p9_session_send_getattr(&s, g_buf, sizeof(g_buf), 88, 0);
    TEST_EXPECT_EQ(rc, -1, "getattr on unbound fid refused");
    struct p9_setattr sa = { .valid = 0 };
    rc = p9_session_send_setattr(&s, g_buf, sizeof(g_buf), 88, &sa);
    TEST_EXPECT_EQ(rc, -1, "setattr on unbound fid refused");
    rc = p9_session_send_readdir(&s, g_buf, sizeof(g_buf), 88, 0, 4096);
    TEST_EXPECT_EQ(rc, -1, "readdir on unbound fid refused");
    rc = p9_session_send_statfs(&s, g_buf, sizeof(g_buf), 88);
    TEST_EXPECT_EQ(rc, -1, "statfs on unbound fid refused");
    rc = p9_session_send_fsync(&s, g_buf, sizeof(g_buf), 88, 0);
    TEST_EXPECT_EQ(rc, -1, "fsync on unbound fid refused");
}

// =============================================================================
// Mutation family (P5-wire-mutation): end-to-end tests.
// =============================================================================

static int synth_empty_r2(u8 *out, size_t cap, u8 type, u16 tag) {
    return synth_rmsg(out, cap, type, tag, NULL, 0);
}

static int synth_qid_r2(u8 *out, size_t cap, u8 type, u16 tag,
                          u8 qtype, u32 qver, u64 qpath) {
    u8 body[P9_QID_LEN];
    body[0] = qtype;
    for (int i = 0; i < 4; i++) body[1 + i] = (u8)((qver >> (i * 8)) & 0xff);
    for (int i = 0; i < 8; i++) body[5 + i] = (u8)((qpath >> (i * 8)) & 0xff);
    return synth_rmsg(out, cap, type, tag, body, sizeof(body));
}

static int synth_readlink(u8 *out, size_t cap, u16 tag,
                            const u8 *target, u16 target_len) {
    u8 body[256];
    if ((size_t)target_len + 2 > sizeof(body)) return -1;
    body[0] = (u8)(target_len & 0xff);
    body[1] = (u8)((target_len >> 8) & 0xff);
    for (u16 i = 0; i < target_len; i++) body[2 + i] = target[i];
    return synth_rmsg(out, cap, P9_RREADLINK, tag, body, (size_t)(2 + target_len));
}

void test_9p_session_symlink_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 40), 0, "walk to fid 40");

    const u8 name[]   = {'a'};
    const u8 symtgt[] = {'t', 'g', 't'};
    int len = p9_session_send_symlink(&s, g_buf, sizeof(g_buf),
                                       40, name, sizeof(name),
                                       symtgt, sizeof(symtgt), /*gid*/ 0);
    TEST_ASSERT(len > 0, "send_symlink ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TSYMLINK, "Tsymlink type");

    len = synth_qid_r2(g_buf, sizeof(g_buf), P9_RSYMLINK, tag,
                        P9_QTSYMLINK, 0, 100);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rsymlink ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TSYMLINK,        "result kind = TSYMLINK");
    TEST_EXPECT_EQ((u64)r.created_qid.type, (u64)P9_QTSYMLINK,
                                                         "Rsymlink created_qid.type");
    TEST_EXPECT_EQ(r.created_qid.path, (u64)100,         "Rsymlink created_qid.path");
}

void test_9p_session_mknod_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 41), 0, "walk to fid 41");

    const u8 name[] = {'n'};
    int len = p9_session_send_mknod(&s, g_buf, sizeof(g_buf),
                                     41, name, sizeof(name),
                                     /*mode*/ 020666u, 1, 3, /*gid*/ 0);
    TEST_ASSERT(len > 0, "send_mknod ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TMKNOD, "Tmknod type");

    len = synth_qid_r2(g_buf, sizeof(g_buf), P9_RMKNOD, tag,
                        P9_QTFILE, 0, 200);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                          "dispatch Rmknod ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TMKNOD,    "result kind = TMKNOD");
    TEST_EXPECT_EQ(r.created_qid.path, (u64)200,   "Rmknod created_qid.path");
}

void test_9p_session_rename_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 42), 0, "walk to fid 42");
    TEST_EXPECT_EQ(walk_to(&s, 43), 0, "walk to fid 43 (target dir)");

    const u8 name[] = {'n', 'e', 'w'};
    int len = p9_session_send_rename(&s, g_buf, sizeof(g_buf),
                                      /*fid*/ 42, /*dfid*/ 43,
                                      name, sizeof(name));
    TEST_ASSERT(len > 0, "send_rename ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TRENAME, "Trename type");

    len = synth_empty_r2(g_buf, sizeof(g_buf), P9_RRENAME, tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                          "dispatch Rrename ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TRENAME,   "result kind = TRENAME");
    TEST_ASSERT(p9_session_fid_bound(&s, 42),      "fid 42 still bound after Rrename");
}

void test_9p_session_readlink_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 44), 0, "walk to fid 44");

    int len = p9_session_send_readlink(&s, g_buf, sizeof(g_buf), 44);
    TEST_ASSERT(len > 0, "send_readlink ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TREADLINK, "Treadlink type");

    const u8 tgt[] = {'/', 't', 'g', 't'};
    len = synth_readlink(g_buf, sizeof(g_buf), tag, tgt, (u16)sizeof(tgt));
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                          "dispatch Rreadlink ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TREADLINK,                 "result kind = TREADLINK");
    TEST_EXPECT_EQ((u64)r.readlink_target_len, (u64)sizeof(tgt),   "Rreadlink target len");
    TEST_ASSERT(r.readlink_target != NULL,                         "Rreadlink target non-null");
    for (size_t i = 0; i < sizeof(tgt); i++) {
        TEST_ASSERT(r.readlink_target[i] == tgt[i], "Rreadlink target byte mismatch");
    }
}

void test_9p_session_link_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 45), 0, "walk to fid 45");
    TEST_EXPECT_EQ(walk_to(&s, 46), 0, "walk to fid 46");

    const u8 name[] = {'a'};
    int len = p9_session_send_link(&s, g_buf, sizeof(g_buf),
                                    /*dfid*/ 45, /*fid*/ 46,
                                    name, sizeof(name));
    TEST_ASSERT(len > 0, "send_link ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TLINK, "Tlink type");

    len = synth_empty_r2(g_buf, sizeof(g_buf), P9_RLINK, tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                       "dispatch Rlink ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TLINK,  "result kind = TLINK");
}

void test_9p_session_mkdir_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 47), 0, "walk to fid 47");

    const u8 name[] = {'d'};
    int len = p9_session_send_mkdir(&s, g_buf, sizeof(g_buf),
                                     47, name, sizeof(name),
                                     /*mode*/ 0755, /*gid*/ 0);
    TEST_ASSERT(len > 0, "send_mkdir ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TMKDIR, "Tmkdir type");

    len = synth_qid_r2(g_buf, sizeof(g_buf), P9_RMKDIR, tag,
                        P9_QTDIR, 0, 300);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                                "dispatch Rmkdir ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TMKDIR,          "result kind = TMKDIR");
    TEST_EXPECT_EQ((u64)r.created_qid.type, (u64)P9_QTDIR, "Rmkdir created_qid.type");
    TEST_EXPECT_EQ(r.created_qid.path, (u64)300,         "Rmkdir created_qid.path");
}

void test_9p_session_renameat_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 48), 0, "walk to fid 48");
    TEST_EXPECT_EQ(walk_to(&s, 49), 0, "walk to fid 49");

    const u8 oldn[] = {'o'};
    const u8 newn[] = {'n'};
    int len = p9_session_send_renameat(&s, g_buf, sizeof(g_buf),
                                        48, oldn, sizeof(oldn),
                                        49, newn, sizeof(newn));
    TEST_ASSERT(len > 0, "send_renameat ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TRENAMEAT, "Trenameat type");

    len = synth_empty_r2(g_buf, sizeof(g_buf), P9_RRENAMEAT, tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                              "dispatch Rrenameat ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TRENAMEAT,     "result kind = TRENAMEAT");
}

void test_9p_session_unlinkat_round_trip(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 50), 0, "walk to fid 50");

    const u8 name[] = {'x'};
    int len = p9_session_send_unlinkat(&s, g_buf, sizeof(g_buf),
                                        50, name, sizeof(name), /*flags*/ 0);
    TEST_ASSERT(len > 0, "send_unlinkat ok");
    u32 sz; u8 ty; u16 tag;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &tag);
    TEST_EXPECT_EQ((u64)ty, (u64)P9_TUNLINKAT, "Tunlinkat type");

    len = synth_empty_r2(g_buf, sizeof(g_buf), P9_RUNLINKAT, tag);
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)len, &r);
    TEST_EXPECT_EQ(rc, 0,                              "dispatch Runlinkat ok");
    TEST_EXPECT_EQ((u64)r.kind, (u64)P9_TUNLINKAT,     "result kind = TUNLINKAT");
}

void test_9p_session_rename_with_inflight_on_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 51), 0, "walk to fid 51");
    TEST_EXPECT_EQ(walk_to(&s, 52), 0, "walk to fid 52");

    const u8 name[] = {'a'};
    int len = p9_session_send_rename(&s, g_buf, sizeof(g_buf),
                                      51, 52, name, sizeof(name));
    TEST_ASSERT(len > 0, "first rename accepted");
    int rc = p9_session_send_rename(&s, g_buf, sizeof(g_buf),
                                     51, 52, name, sizeof(name));
    TEST_EXPECT_EQ(rc, -1, "concurrent rename on same fid refused");
}

void test_9p_session_unlinkat_permits_concurrent(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    TEST_EXPECT_EQ(walk_to(&s, 53), 0, "walk to fid 53");

    const u8 n1[] = {'a'};
    const u8 n2[] = {'b'};
    int len1 = p9_session_send_unlinkat(&s, g_buf, sizeof(g_buf),
                                         53, n1, sizeof(n1), 0);
    TEST_ASSERT(len1 > 0, "first unlinkat accepted");
    static u8 buf2[1024];
    int len2 = p9_session_send_unlinkat(&s, buf2, sizeof(buf2),
                                         53, n2, sizeof(n2), 0);
    TEST_ASSERT(len2 > 0, "concurrent unlinkat on same dfid (different name) accepted");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2, "two unlinkats inflight");
}

void test_9p_session_mutation_from_unbound_fid_refused(void) {
    struct p9_session s;
    drive_session_open(&s, 0);
    // Fid 88 never bound.
    const u8 name[] = {'x'};
    int rc;
    rc = p9_session_send_symlink(&s, g_buf, sizeof(g_buf), 88,
                                   name, sizeof(name), name, sizeof(name), 0);
    TEST_EXPECT_EQ(rc, -1, "symlink on unbound fid refused");
    rc = p9_session_send_mknod(&s, g_buf, sizeof(g_buf), 88,
                                name, sizeof(name), 0, 0, 0, 0);
    TEST_EXPECT_EQ(rc, -1, "mknod on unbound dfid refused");
    rc = p9_session_send_rename(&s, g_buf, sizeof(g_buf), 88, 0,
                                 name, sizeof(name));
    TEST_EXPECT_EQ(rc, -1, "rename on unbound fid refused");
    rc = p9_session_send_readlink(&s, g_buf, sizeof(g_buf), 88);
    TEST_EXPECT_EQ(rc, -1, "readlink on unbound fid refused");
    rc = p9_session_send_link(&s, g_buf, sizeof(g_buf), 88, 0,
                               name, sizeof(name));
    TEST_EXPECT_EQ(rc, -1, "link on unbound dfid refused");
    rc = p9_session_send_mkdir(&s, g_buf, sizeof(g_buf), 88,
                                name, sizeof(name), 0, 0);
    TEST_EXPECT_EQ(rc, -1, "mkdir on unbound dfid refused");
    rc = p9_session_send_renameat(&s, g_buf, sizeof(g_buf),
                                    88, name, sizeof(name),
                                    88, name, sizeof(name));
    TEST_EXPECT_EQ(rc, -1, "renameat on unbound olddirfid refused");
    rc = p9_session_send_unlinkat(&s, g_buf, sizeof(g_buf), 88,
                                    name, sizeof(name), 0);
    TEST_EXPECT_EQ(rc, -1, "unlinkat on unbound dfid refused");
}

// =============================================================================
// Tflush abandon -> reclaim (#845). The session-layer core of the fix: a
// Tflush reserves the abandoned tag, and the Rflush is the SOLE authority that
// frees it. These deterministically exercise every line of the session-side
// fix; the live client_run DIED -> Tflush integration under a real dying Proc
// (a survivor's reader draining the Rflush) is the SMP window the synchronous
// harness cannot produce -- OWED with the A-5b multi-in-flight workload, same
// as the #841 elected-reader coverage gap.
// =============================================================================

void test_9p_session_flush_reclaims_both(void) {
    struct p9_session s;
    TEST_EXPECT_EQ(drive_session_open(&s, /*root_fid=*/0), 0, "drive_session_open");

    // One in-flight op on the bound root fid (read-shaped, concurrent-OK).
    int len = p9_session_send_getattr(&s, g_buf, sizeof(g_buf), 0, P9_GETATTR_BASIC);
    TEST_ASSERT(len > 0, "send_getattr ok");
    u32 sz; u8 ty; u16 t;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &t);
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)1, "1 inflight (the op)");

    // Abandon T: send_flush allocates a distinct flush tag F and reserves T.
    len = p9_session_send_flush(&s, g_buf, sizeof(g_buf), t);
    TEST_ASSERT(len > 0, "send_flush ok");
    u32 sz2; u8 ty2; u16 fT;
    p9_peek_header(g_buf, (size_t)len, &sz2, &ty2, &fT);
    TEST_EXPECT_EQ((u64)ty2, (u64)P9_TFLUSH, "flush op is Tflush");
    TEST_ASSERT((u64)fT != (u64)t, "flush tag distinct from oldtag");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2, "2 inflight (op + flush)");

    // Re-flushing the same oldtag is refused (already awaiting).
    int dup = p9_session_send_flush(&s, g_buf, sizeof(g_buf), t);
    TEST_EXPECT_EQ(dup, -1, "double-flush of the same oldtag refused");

    // Server Rflushes F -> frees BOTH the flush tag and the abandoned oldtag.
    int rlen = synth_rmsg(g_buf, sizeof(g_buf), P9_RFLUSH, fT, NULL, 0);
    TEST_ASSERT(rlen > 0, "synth Rflush");
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)rlen, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rflush ok");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "both tags reclaimed by Rflush");

    // The reclaimed tag space is reusable AND a fresh op completes end-to-end
    // on it (F2): walk root -> fid 7 on the reused tag, dispatch its Rwalk,
    // assert the op completes (inflight back to 0) and the fid binds. A Tflush
    // that failed to FULLY clear the reserved tag would leave it un-reusable or
    // mis-complete here -- the genuine reclaim-then-reuse proof.
    len = p9_session_send_walk(&s, g_buf, sizeof(g_buf), /*src=*/0, /*new=*/7,
                                /*nwname=*/0, NULL, NULL);
    TEST_ASSERT(len > 0, "post-flush walk reuses the freed tag");
    u32 sz3; u8 ty3; u16 wt;
    p9_peek_header(g_buf, (size_t)len, &sz3, &ty3, &wt);
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)1, "1 inflight after reuse");
    int wlen = synth_rwalk_single(g_buf, sizeof(g_buf), wt, P9_QTDIR, 1, 7);
    TEST_ASSERT(wlen > 0, "synth Rwalk for reused tag");
    rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)wlen, &r);
    TEST_EXPECT_EQ(rc, 0, "reused-tag op completes end-to-end");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "reused op reclaimed cleanly");
    TEST_ASSERT(p9_session_fid_bound(&s, 7), "reused-tag walk bound its fid");

    p9_session_destroy(&s);
}

void test_9p_session_late_reply_does_not_free_awaiting_flush(void) {
    struct p9_session s;
    TEST_EXPECT_EQ(drive_session_open(&s, /*root_fid=*/0), 0, "drive_session_open");

    int len = p9_session_send_getattr(&s, g_buf, sizeof(g_buf), 0, P9_GETATTR_BASIC);
    TEST_ASSERT(len > 0, "send_getattr ok");
    u32 sz; u8 ty; u16 t;
    p9_peek_header(g_buf, (size_t)len, &sz, &ty, &t);

    // Abandon T (flush F reserves T).
    len = p9_session_send_flush(&s, g_buf, sizeof(g_buf), t);
    TEST_ASSERT(len > 0, "send_flush ok");
    u32 sz2; u8 ty2; u16 fT;
    p9_peek_header(g_buf, (size_t)len, &sz2, &ty2, &fT);
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2, "op + flush in flight");

    // The server answered the original BEFORE seeing the flush: a late reply
    // for T arrives. It MUST be consumed but MUST NOT free T -- 9P forbids
    // reusing oldtag until the Rflush, so freeing here would let the tag be
    // reused while a stray duplicate is still possible (the I-10 reuse-race
    // the naive fix introduces). The early consume-without-clear returns before
    // the kind-specific parse, so a header-only frame suffices to prove the
    // reservation. (If a future change frees T on this path, this test fails.)
    int rlen = synth_rmsg(g_buf, sizeof(g_buf), P9_RGETATTR, t, NULL, 0);
    TEST_ASSERT(rlen > 0, "synth late Rgetattr (header-only)");
    struct p9_dispatch_result r;
    int rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)rlen, &r);
    TEST_EXPECT_EQ(rc, 0, "late original reply consumed");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)2,
                   "T still reserved after late reply -- the I-10 guard");

    // The Rflush is the sole authority that frees the reserved oldtag.
    rlen = synth_rmsg(g_buf, sizeof(g_buf), P9_RFLUSH, fT, NULL, 0);
    rc = p9_session_dispatch_rmsg(&s, g_buf, (size_t)rlen, &r);
    TEST_EXPECT_EQ(rc, 0, "dispatch Rflush ok");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&s), (u64)0, "both reclaimed after Rflush");

    p9_session_destroy(&s);
}
