// p9_attach lifecycle tests (P5-attach-create).
//
// The kernel-internal machinery that the future attach_9p syscall
// handler will call. Verifies the create → root-Spoor → destroy
// lifecycle against a loopback transport.

#include "test.h"

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_p9_attached_create_destroy(void);
void test_p9_attached_handshake_failure_returns_null(void);
void test_p9_attached_root_spoor_walk_read(void);
void test_p9_attached_query_helpers(void);

// File-scope storage for the loopback's response buffer. The attached
// struct heap-allocates everything else (client + recv_buf).
static struct p9_loopback g_loopback;
static u8 g_loopback_resp[4096];

// Canonical responder — synthesizes valid Rmsgs for every op the
// attach + test sequence issues. Mirrors test_dev9p.c's responder.
static int attach_responder(void *ctx, const u8 *req, size_t req_len,
                              u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;

    if (type == P9_TVERSION) {
        size_t total = P9_HDR_LEN + 4 + 2 + 8;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RVERSION;
        resp[5] = 0xff; resp[6] = 0xff;
        resp[7] = 0; resp[8] = 0x20; resp[9] = 0; resp[10] = 0;     // msize=8192
        resp[11] = 8; resp[12] = 0;
        const char *v = "9P2000.L";
        for (int i = 0; i < 8; i++) resp[13 + i] = (u8)v[i];
        return (int)total;
    }
    if (type == P9_TATTACH) {
        size_t total = P9_HDR_LEN + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RATTACH;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTDIR;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 1; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        return (int)total;
    }
    if (type == P9_TWALK) {
        if (req_len < P9_HDR_LEN + 4 + 4 + 2) return -1;
        u16 nwname = (u16)req[15] | ((u16)req[16] << 8);
        size_t body_len = 2 + (size_t)nwname * P9_QID_LEN;
        size_t total = P9_HDR_LEN + body_len;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWALK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(nwname & 0xff); resp[8] = (u8)((nwname >> 8) & 0xff);
        for (u16 i = 0; i < nwname; i++) {
            size_t off = P9_HDR_LEN + 2 + (size_t)i * P9_QID_LEN;
            resp[off] = P9_QTFILE;
            for (int j = 0; j < 4; j++) resp[off + 1 + j] = 0;
            resp[off + 5] = (u8)(0x20 + i);
            for (int j = 1; j < 8; j++) resp[off + 5 + j] = 0;
        }
        return (int)total;
    }
    if (type == P9_TCLUNK) {
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RCLUNK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    if (type == P9_TLOPEN) {
        size_t total = P9_HDR_LEN + P9_QID_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RLOPEN;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTFILE;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 0x55; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        resp[20] = 0; resp[21] = 0x10; resp[22] = 0; resp[23] = 0;
        return (int)total;
    }
    if (type == P9_TREAD) {
        const u8 payload[] = {'a','t','t','a','c','h'};
        size_t total = P9_HDR_LEN + 4 + sizeof(payload);
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREAD;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)sizeof(payload); resp[8] = 0; resp[9] = 0; resp[10] = 0;
        for (size_t i = 0; i < sizeof(payload); i++) resp[11 + i] = payload[i];
        return (int)total;
    }
    return -1;
}

// Responder that surfaces Rlerror for Tattach (Tversion succeeds first).
// Used to test handshake-failure cleanup.
static int handshake_fail_responder(void *ctx, const u8 *req, size_t req_len,
                                       u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if (type == P9_TVERSION) {
        // Use the canonical responder for Tversion.
        return attach_responder(ctx, req, req_len, resp, resp_cap);
    }
    if (type == P9_TATTACH) {
        // Return Rlerror with ecode=13 (EACCES).
        size_t total = P9_HDR_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RLERROR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = 13; resp[8] = 0; resp[9] = 0; resp[10] = 0;
        return (int)total;
    }
    return -1;
}

// =============================================================================
// Tests.
// =============================================================================

void test_p9_attached_create_destroy(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                attach_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init");

    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback),
        /*recv_cap=*/4096,
        /*root_fid=*/0, /*msize=*/8192,
        uname, sizeof(uname), aname, sizeof(aname), 0);
    TEST_ASSERT(a != NULL,                       "attached created");
    TEST_ASSERT(p9_attached_is_open(a),          "session is OPEN");
    TEST_EXPECT_EQ((u64)a->root_fid, (u64)0,     "root_fid = 0");

    p9_attached_destroy(a);
    p9_loopback_destroy(&g_loopback);
}

void test_p9_attached_handshake_failure_returns_null(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                handshake_fail_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (handshake_fail)");

    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback),
        4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0);
    TEST_ASSERT(a == NULL, "attached creation fails on Rattach Rlerror");
    // No leak — p9_attached_create cleaned up the half-built client +
    // buffers on the handshake failure. Verifiable indirectly:
    // subsequent attempts to allocate succeed.
    p9_loopback_destroy(&g_loopback);
}

void test_p9_attached_root_spoor_walk_read(void) {
    p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                       attach_responder, NULL);
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback), 4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0);
    TEST_ASSERT(a != NULL, "attached created");

    // Get the root Spoor + walk + open + read end-to-end.
    struct Spoor *root = p9_attached_root_spoor(a);
    TEST_ASSERT(root != NULL,                    "root Spoor produced");
    TEST_EXPECT_EQ((u64)root->dc, (u64)DEV9P_DC, "root is dev9p-backed");

    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL,                       "walk through attached succeeded");
    walkqid_free(w);

    struct Spoor *opened = dev9p.open(nc, 0);
    TEST_ASSERT(opened == nc,                    "open succeeded");

    u8 buf[64];
    long got = dev9p.read(nc, buf, 64, 0);
    TEST_EXPECT_EQ((u64)got, (u64)6,             "read returned 6 bytes (attach)");
    TEST_ASSERT(buf[0] == 'a' && buf[5] == 'h',  "payload bytes match");

    spoor_clunk(nc);
    spoor_clunk(root);
    p9_attached_destroy(a);
    p9_loopback_destroy(&g_loopback);
}

void test_p9_attached_query_helpers(void) {
    p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                       attach_responder, NULL);
    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback), 4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0);
    TEST_ASSERT(a != NULL, "attached");

    TEST_ASSERT(p9_attached_is_open(a),          "is_open after create");
    TEST_ASSERT(!p9_attached_is_open(NULL),      "is_open(NULL) = false");

    p9_attached_destroy(a);
    // After destroy, `a` is freed — we can't call is_open on it
    // safely. Sanity-check that NULL still returns false.
    TEST_ASSERT(!p9_attached_is_open(NULL),      "is_open(NULL) still false post-destroy");
    p9_loopback_destroy(&g_loopback);
}
