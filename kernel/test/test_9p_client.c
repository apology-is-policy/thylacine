// 9P client high-level API tests (P5-client).
//
// The wire + session + transport layers each have their own test
// suites; these tests verify the COMPOSITION — that each high-level
// op correctly chains session.send + transport.exchange + result
// extraction + error mapping.
//
// One representative test per op category + lifecycle + handshake +
// error-propagation through Rlerror.

#include "test.h"

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_9p_client_init_destroy(void);
void test_9p_client_handshake(void);
void test_9p_client_walk_and_clunk(void);
void test_9p_client_lopen_read(void);
void test_9p_client_write(void);
void test_9p_client_getattr(void);
void test_9p_client_readdir(void);
void test_9p_client_statfs(void);
void test_9p_client_mkdir(void);
void test_9p_client_unlinkat(void);
void test_9p_client_readlink(void);
void test_9p_client_rlerror_propagates_to_negative_errno(void);
void test_9p_client_op_before_handshake_returns_ebusy(void);
void test_9p_client_lock_released_between_ops(void);
void test_9p_client_async_op_posts_cqe(void);
void test_9p_client_async_session_death_posts_error_cqe(void);
void test_9p_client_async_handoff_skips_async(void);
void test_9p_client_pump_deadline_idle(void);
void test_9p_client_pump_deadline_data_ready_progresses(void);
void test_9p_client_pump_deadline_chunked_frame_completes(void);
void test_9p_client_pump_deadline_busy_when_reader_active(void);
void test_9p_client_loom_fsync_e2e(void);
void test_9p_client_loom_rights_deny(void);
void test_9p_client_loom_quiesce_abandons_inflight(void);
void test_9p_client_loom_multishot_stream(void);
void test_9p_client_loom_multishot_backpressure(void);

// File-scope buffers (kernel test stack is 16 KiB — client struct is
// ~4 KiB; multiple in one frame is fine but file-scope is cleaner).
static u8 g_recv_buf[4096];
static u8 g_loopback_resp[4096];

// Reusable responder that handles every Tmsg type with a sensible
// canned response. Tests choose which fid path / qid the server
// returns by reading the request opcode + tag.
static int canonical_responder(void *ctx, const u8 *req, size_t req_len,
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
        resp[5] = 0xff; resp[6] = 0xff;     // NOTAG
        resp[7] = 0; resp[8] = 0x20; resp[9] = 0; resp[10] = 0;   // msize=8192
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
        resp[12] = 42; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        return (int)total;
    }
    if (type == P9_TWALK) {
        // Always 1 qid back regardless of nwname requested.
        size_t total = P9_HDR_LEN + 2 + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWALK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = 1; resp[8] = 0;            // nwqid = 1
        resp[9] = P9_QTFILE;
        for (int i = 0; i < 4; i++) resp[10 + i] = 0;
        resp[14] = 77; for (int i = 1; i < 8; i++) resp[14 + i] = 0;
        return (int)total;
    }
    if (type == P9_TCLUNK || type == P9_TSETATTR || type == P9_TFSYNC ||
        type == P9_TRENAME || type == P9_TRENAMEAT || type == P9_TLINK ||
        type == P9_TUNLINKAT || type == P9_TFLUSH) {
        // Empty body (Rflush is header-only too -- P9_RFLUSH == P9_TFLUSH+1, so
        // the type+1 reply below is a valid Rflush; the Loom-3 quiesce test
        // exercises the abandon Tflush -> Rflush path).
        size_t total = P9_HDR_LEN;
        if (resp_cap < total) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = (u8)(type + 1);
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)total;
    }
    if (type == P9_TLOPEN || type == P9_TLCREATE) {
        // qid + iounit.
        size_t total = P9_HDR_LEN + P9_QID_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = (u8)(type + 1);
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTFILE;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 99; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        resp[20] = 0x00; resp[21] = 0x10; resp[22] = 0; resp[23] = 0;  // iounit=4096
        return (int)total;
    }
    if (type == P9_TREAD) {
        // count=5 with payload "hello".
        const u8 payload[] = {'h','e','l','l','o'};
        size_t total = P9_HDR_LEN + 4 + sizeof(payload);
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREAD;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)sizeof(payload); resp[8] = 0; resp[9] = 0; resp[10] = 0;
        for (size_t i = 0; i < sizeof(payload); i++) resp[11 + i] = payload[i];
        return (int)total;
    }
    if (type == P9_TWRITE) {
        // Echo the requested count back as accepted-count.
        // Request body: fid(4) + offset(8) + count(4) + data(count).
        if (req_len < P9_HDR_LEN + 4 + 8 + 4) return -1;
        u32 count = (u32)req[P9_HDR_LEN + 4 + 8]
                  | ((u32)req[P9_HDR_LEN + 4 + 8 + 1] << 8)
                  | ((u32)req[P9_HDR_LEN + 4 + 8 + 2] << 16)
                  | ((u32)req[P9_HDR_LEN + 4 + 8 + 3] << 24);
        size_t total = P9_HDR_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWRITE;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(count & 0xff);
        resp[8] = (u8)((count >> 8) & 0xff);
        resp[9] = (u8)((count >> 16) & 0xff);
        resp[10] = (u8)((count >> 24) & 0xff);
        return (int)total;
    }
    if (type == P9_TGETATTR) {
        // Minimum statx-shape response (153-byte body).
        size_t body_len = 8 + P9_QID_LEN + 4 * 3 + 8 * 15;
        size_t total = P9_HDR_LEN + body_len;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff);
        resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RGETATTR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        size_t off = P9_HDR_LEN;
        // valid = P9_GETATTR_BASIC
        u64 valid = P9_GETATTR_BASIC;
        for (int i = 0; i < 8; i++) resp[off + i] = (u8)((valid >> (i * 8)) & 0xff);
        off += 8;
        // qid: type / version / path
        resp[off] = P9_QTFILE; off += 1;
        for (int i = 0; i < 4; i++) resp[off + i] = 0;  off += 4;     // version
        resp[off] = 55; for (int i = 1; i < 8; i++) resp[off + i] = 0; off += 8;  // path=55
        // mode/uid/gid = 0644 / 0 / 0
        resp[off] = 0xA4; resp[off+1] = 0x01; resp[off+2] = 0; resp[off+3] = 0; off += 4;
        for (int i = 0; i < 8; i++) resp[off + i] = 0; off += 8;       // uid + gid
        // 5 u64 mid + 8 u64 times + 2 u64 trailing = 15 u64 (120 bytes)
        for (int i = 0; i < 120; i++) resp[off + i] = 0;
        // size at offset 16 of mid block; just set size=128
        size_t size_off = P9_HDR_LEN + 8 + P9_QID_LEN + 12 + 16;
        resp[size_off] = 0x80; for (int i = 1; i < 8; i++) resp[size_off + i] = 0;
        return (int)total;
    }
    if (type == P9_TREADDIR) {
        // Empty dirent stream (count=0).
        size_t total = P9_HDR_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREADDIR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = 0; resp[8] = 0; resp[9] = 0; resp[10] = 0;
        return (int)total;
    }
    if (type == P9_TSTATFS) {
        size_t total = P9_HDR_LEN + 60;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RSTATFS;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        for (int i = 0; i < 60; i++) resp[P9_HDR_LEN + i] = 0;
        // type (4) + bsize (4) -- set bsize = 4096
        resp[P9_HDR_LEN + 4] = 0; resp[P9_HDR_LEN + 5] = 0x10;
        return (int)total;
    }
    if (type == P9_TSYMLINK || type == P9_TMKNOD || type == P9_TMKDIR) {
        // Qid-only response.
        size_t total = P9_HDR_LEN + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = (u8)(type + 1);
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        u8 qt = (type == P9_TMKDIR) ? P9_QTDIR :
                (type == P9_TSYMLINK) ? P9_QTSYMLINK : P9_QTFILE;
        resp[7] = qt;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 88; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        return (int)total;
    }
    if (type == P9_TREADLINK) {
        const u8 tgt[] = {'/','t','m','p'};
        size_t total = P9_HDR_LEN + 2 + sizeof(tgt);
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREADLINK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)sizeof(tgt); resp[8] = 0;
        for (size_t i = 0; i < sizeof(tgt); i++) resp[9 + i] = tgt[i];
        return (int)total;
    }
    return -1;
}

// Responder that always returns Rlerror with a fixed ecode.
static int rlerror_responder(void *ctx, const u8 *req, size_t req_len,
                               u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if (type == P9_TVERSION) {
        // Special-case: Tversion is out-of-band; can't Rlerror it.
        // Return a normal Rversion.
        return canonical_responder(ctx, req, req_len, resp, resp_cap);
    }
    if (type == P9_TATTACH) {
        // Same — give Rattach so handshake completes.
        return canonical_responder(ctx, req, req_len, resp, resp_cap);
    }
    // Anything else: Rlerror with ecode = 2 (ENOENT).
    size_t total = P9_HDR_LEN + 4;
    if (resp_cap < total) return -1;
    resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
    resp[4] = P9_RLERROR;
    resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
    resp[7] = 2; resp[8] = 0; resp[9] = 0; resp[10] = 0;
    return (int)total;
}

// Helper: initialize a client with the canonical responder, drive
// handshake, leave the client ready for ops. Caller-provided storage.
static int drive_client_open(struct p9_client *c, struct p9_loopback *lb) {
    int rc = p9_loopback_init(lb, g_loopback_resp, sizeof(g_loopback_resp),
                                canonical_responder, NULL);
    if (rc < 0) return -1;
    rc = p9_client_init(c, /*root_fid=*/0, /*msize=*/8192,
                         p9_loopback_ops_for(lb),
                         g_recv_buf, sizeof(g_recv_buf));
    if (rc < 0) return -1;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    return p9_client_handshake(c, uname, sizeof(uname), aname, sizeof(aname), 0);
}

// =============================================================================
// Tests.
// =============================================================================

// Static client storage at file scope — struct p9_client is ~4 KiB +
// the embedded session is ~4 KiB; declaring on the stack risks
// overflow on the 16 KiB test thread stack.
static struct p9_client g_client;
static struct p9_loopback g_loopback;

void test_9p_client_init_destroy(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init");

    rc = p9_client_init(&g_client, 0, 8192,
                         p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    TEST_ASSERT(!p9_client_is_open(&g_client),
                 "client not open before handshake (session in INIT)");

    p9_client_destroy(&g_client);
    TEST_EXPECT_EQ((u32)g_client.magic, (u32)0, "destroy clobbers magic");
    p9_loopback_destroy(&g_loopback);

    // Invalid args refused.
    rc = p9_client_init(NULL, 0, 8192, p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, -P9_E_INVAL, "init NULL refused");
}

void test_9p_client_handshake(void) {
    TEST_EXPECT_EQ(drive_client_open(&g_client, &g_loopback), 0,
                    "handshake completes");
    TEST_ASSERT(p9_client_is_open(&g_client), "client is OPEN after handshake");
    TEST_EXPECT_EQ((u64)g_client.total_ops, (u64)2,
                    "handshake = 2 ops (version + attach)");
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_walk_and_clunk(void) {
    drive_client_open(&g_client, &g_loopback);

    // Walk root → fid 5 (clone, no names).
    struct p9_qid qids[P9_MAX_WALK];
    u16 nwqid;
    int rc = p9_client_walk(&g_client, /*src=*/0, /*new=*/5,
                              /*nwname=*/0, NULL, NULL, &nwqid, qids);
    TEST_EXPECT_EQ(rc, 0, "walk(0→5, clone) ok");
    TEST_EXPECT_EQ((u64)nwqid, (u64)1, "1 qid returned");

    // Walk-one convenience.
    const u8 name[] = {'a'};
    struct p9_qid q;
    rc = p9_client_walk_one(&g_client, /*src=*/0, /*new=*/6,
                              name, sizeof(name), &q);
    TEST_EXPECT_EQ(rc, 0,                              "walk_one(0→6) ok");
    TEST_EXPECT_EQ(q.path, (u64)77,                    "walked qid.path = 77");

    rc = p9_client_clunk(&g_client, 5);
    TEST_EXPECT_EQ(rc, 0, "clunk fid 5 ok");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_lopen_read(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 10, (const u8 *)"f", 1, NULL);

    struct p9_qid q;
    u32 iounit;
    int rc = p9_client_lopen(&g_client, 10, /*flags=*/0, &q, &iounit);
    TEST_EXPECT_EQ(rc, 0,                          "lopen ok");
    TEST_EXPECT_EQ((u64)iounit, (u64)4096,         "iounit round-trip");

    u8 data[64];
    u32 n;
    rc = p9_client_read(&g_client, 10, /*offset=*/0, /*count=*/64, data, &n);
    TEST_EXPECT_EQ(rc, 0,                          "read ok");
    TEST_EXPECT_EQ((u64)n, (u64)5,                 "read count = 5 (hello)");
    TEST_ASSERT(data[0] == 'h' && data[4] == 'o',  "read payload matches");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_write(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 11, (const u8 *)"f", 1, NULL);

    const u8 payload[] = {'w', 'r', 'i', 't', 'e'};
    u32 accepted;
    int rc = p9_client_write(&g_client, 11, 0,
                               (u32)sizeof(payload), payload, &accepted);
    TEST_EXPECT_EQ(rc, 0,                                  "write ok");
    TEST_EXPECT_EQ((u64)accepted, (u64)sizeof(payload),    "accepted = sent");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_getattr(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 12, (const u8 *)"f", 1, NULL);

    struct p9_attr attr;
    int rc = p9_client_getattr(&g_client, 12, P9_GETATTR_BASIC, &attr);
    TEST_EXPECT_EQ(rc, 0,                         "getattr ok");
    TEST_EXPECT_EQ((u64)attr.mode, (u64)0644,     "mode round-trip");
    TEST_EXPECT_EQ(attr.size, (u64)128,           "size round-trip");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_readdir(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 13, (const u8 *)"d", 1, NULL);

    u8 buf[256];
    u32 n;
    int rc = p9_client_readdir(&g_client, 13, 0, sizeof(buf), buf, &n);
    TEST_EXPECT_EQ(rc, 0,            "readdir ok");
    TEST_EXPECT_EQ((u64)n, (u64)0,   "empty dir → count 0");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_statfs(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 14, (const u8 *)"d", 1, NULL);

    struct p9_statfs sf;
    int rc = p9_client_statfs(&g_client, 14, &sf);
    TEST_EXPECT_EQ(rc, 0,                       "statfs ok");
    TEST_EXPECT_EQ((u64)sf.bsize, (u64)4096,    "bsize round-trip");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_mkdir(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 15, (const u8 *)"d", 1, NULL);

    const u8 name[] = {'s', 'u', 'b'};
    struct p9_qid q;
    int rc = p9_client_mkdir(&g_client, 15, name, sizeof(name),
                              /*mode=*/0755, /*gid=*/0, &q);
    TEST_EXPECT_EQ(rc, 0,                          "mkdir ok");
    TEST_EXPECT_EQ((u64)q.type, (u64)P9_QTDIR,     "mkdir qid.type = DIR");
    TEST_EXPECT_EQ(q.path, (u64)88,                "mkdir qid.path = 88");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_unlinkat(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 16, (const u8 *)"d", 1, NULL);

    const u8 name[] = {'r', 'm'};
    int rc = p9_client_unlinkat(&g_client, 16, name, sizeof(name), 0);
    TEST_EXPECT_EQ(rc, 0, "unlinkat ok");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

void test_9p_client_readlink(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 17, (const u8 *)"s", 1, NULL);

    u8 target[256];
    u16 target_len = sizeof(target);
    int rc = p9_client_readlink(&g_client, 17, target, &target_len);
    TEST_EXPECT_EQ(rc, 0,                          "readlink ok");
    TEST_EXPECT_EQ((u64)target_len, (u64)4,        "target len = 4 (/tmp)");
    TEST_ASSERT(target[0] == '/' && target[3] == 'p', "target = /tmp");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Rlerror responder returns ecode=2 (ENOENT) for every op except
// version/attach. The client must surface -2.
void test_9p_client_rlerror_propagates_to_negative_errno(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                rlerror_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (rlerror)");

    rc = p9_client_init(&g_client, 0, 8192,
                         p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");

    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    rc = p9_client_handshake(&g_client, uname, sizeof(uname),
                               aname, sizeof(aname), 0);
    TEST_EXPECT_EQ(rc, 0, "handshake ok (server gives normal Rversion + Rattach)");

    // Now any op should surface -ENOENT (= -2).
    rc = p9_client_walk_one(&g_client, 0, 5, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -2, "walk Rlerror → -ENOENT");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Calling an op before handshake (session in INIT state) should
// return -EBUSY since p9_client_is_open returns false.
void test_9p_client_op_before_handshake_returns_ebusy(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init");

    rc = p9_client_init(&g_client, 0, 8192,
                         p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");

    // No handshake yet; client.session.state == INIT.
    rc = p9_client_walk_one(&g_client, 0, 5, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -P9_E_BUSY, "walk before handshake → -EBUSY");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// R15-c F230 regression: verify the per-client spin_lock is properly
// acquired and released around every public op. Single-CPU at v1.0 so
// the spin part is a no-op, but the acquire/release plumbing matters:
// a missed release would leave c->lock.value at 1 after the op, which
// would deadlock the NEXT op (assuming SMP). The clean state between
// ops is the structural witness. SMP race detection awaits TSan.
void test_9p_client_lock_released_between_ops(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init");

    rc = p9_client_init(&g_client, 0, 8192,
                         p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock unlocked at init");

    drive_client_open(&g_client, &g_loopback);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after handshake");

    // Walk + clunk sequence — exercises walk + clunk lock paths.
    p9_client_walk_one(&g_client, 0, 5, (const u8 *)"a", 1, NULL);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after walk");

    p9_client_clunk(&g_client, 5);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after clunk");

    // alloc_fid path also acquires + releases.
    u32 fid1 = p9_client_alloc_fid(&g_client);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after alloc_fid");
    u32 fid2 = p9_client_alloc_fid(&g_client);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after second alloc_fid");
    TEST_ASSERT(fid2 == fid1 + 1, "alloc_fid is monotonic under lock");

    // Diagnostic read path.
    (void)p9_client_is_open(&g_client);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after is_open");
    (void)p9_client_inflight(&g_client);
    TEST_EXPECT_EQ((u64)g_client.lock.value, (u64)0,
                    "lock released after inflight");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-2b: the pluggable-completion seam (POST_CQE).
//
// An async op embeds a p9_rpc with on_complete set. The engine never blocks a
// submitter on it -- when the reply is demuxed (or the session dies) the engine
// invokes on_complete, which posts a CQE into a Loom's CQ ring. These tests
// exercise the seam end-to-end over the loopback: submit_async (no wait) ->
// reader_pump_once (demux) -> on_complete -> loom_post_cqe.
//
// The test OWNS the Loom ref for the whole test (so the callback's lifetime is
// trivial: post + record, never loom_unref). The production async-op container
// + the ref-holding / quiesce-before-free lifetime are Loom-3, where
// SYS_LOOM_ENTER makes the op's ref-drop safe outside the lock.
// =============================================================================

struct test_async_op {
    struct p9_rpc rpc;       // FIRST member -> the callback recovers it by cast
    struct Loom  *loom;
    u64           user_data;
    s32           last_result;
    bool          completed;
};
_Static_assert(__builtin_offsetof(struct test_async_op, rpc) == 0,
               "rpc must be first: the on_complete callback casts rpc -> op");

static struct test_async_op g_async_op;

static void test_async_on_complete(struct p9_rpc *rpc, int status,
                                   struct p9_dispatch_result *dr) {
    struct test_async_op *op = (struct test_async_op *)rpc;   // rpc is first
    (void)dr;   // clunk carries no payload; `status` is the mapped result
    op->last_result = (s32)status;
    op->completed   = true;
    (void)loom_post_cqe(op->loom, op->user_data, (s32)status, 0);
}

// NULL-safe recorder for the handoff-skip test: it must NEVER fire (the handoff
// skips async ops), so it records the invocation without dereferencing a
// container -- a skip regression becomes a clean assertion failure, not a wild
// deref of a bare p9_rpc cast to a test_async_op.
static bool g_handoff_async_fired;
static void test_handoff_async_recorder(struct p9_rpc *rpc, int status,
                                        struct p9_dispatch_result *dr) {
    (void)rpc; (void)status; (void)dr;
    g_handoff_async_fired = true;
}

// Build thunk for submit_async: a Tclunk on the fid passed via ctx.
static int test_build_clunk(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    u32 fid = *(u32 *)ctx;
    return p9_session_send_clunk(s, out, cap, fid);
}

// A demuxed reply drives on_complete, which posts a CQE carrying the op's
// user_data + the mapped (success = 0) result.
void test_9p_client_async_op_posts_cqe(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"f", 1, NULL);  // bind fid 20

    g_async_op.loom        = l;
    g_async_op.user_data   = 0xCAFEBABE12345678ULL;
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    g_async_op.rpc.on_complete = test_async_on_complete;

    u32 fid = 20;
    int rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async(clunk) succeeds (op in flight)");
    TEST_ASSERT(!g_async_op.completed, "not completed before the reader pumps");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)0, "no CQE before pump");

    int pumped = p9_client_reader_pump_once(&g_client);   // recv Rclunk + demux
    TEST_EXPECT_EQ(pumped, 1, "pump demuxed one frame");
    TEST_ASSERT(g_async_op.completed, "on_complete fired");
    TEST_EXPECT_EQ(g_async_op.last_result, 0, "clunk success -> result 0");

    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one CQE posted");
    TEST_EXPECT_EQ(cqes[0].user_data, 0xCAFEBABE12345678ULL, "CQE user_data echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "CQE result = 0 (success)");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
    loom_unref(l);
}

// A session death (transport error) completes an in-flight async op with an
// error CQE -- there is no submitter rendez to wake (mark_dead's async arm).
void test_9p_client_async_session_death_posts_error_cqe(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 21, (const u8 *)"f", 1, NULL);

    g_async_op.loom        = l;
    g_async_op.user_data   = 0xABCDEF01;
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    g_async_op.rpc.on_complete = test_async_on_complete;

    u32 fid = 21;
    int rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async succeeds (op in flight)");
    TEST_ASSERT(!g_async_op.completed, "not yet completed");

    // Break the transport: destroy clears the loopback magic, so the next recv
    // returns -1 -> the reader marks the session dead -> the in-flight async op
    // completes with -EIO (no staged reply is consumed).
    p9_loopback_destroy(&g_loopback);
    int pumped = p9_client_reader_pump_once(&g_client);
    TEST_EXPECT_EQ(pumped, -P9_E_IO, "pump sees the dead transport");
    TEST_ASSERT(g_async_op.completed, "async op completed on session death");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)(-P9_E_IO),
                    "error CQE result = -EIO");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one (error) CQE posted");

    p9_client_destroy(&g_client);   // loopback already destroyed
    loom_unref(l);
}

// The elected-reader handoff hands the role to a pending SYNC op and SKIPS an
// async op (which has no thread to run the reader loop). White-box: inject one
// of each into inflight[] and assert which one is flagged be_reader.
void test_9p_client_async_handoff_skips_async(void) {
    drive_client_open(&g_client, &g_loopback);

    g_handoff_async_fired = false;
    struct p9_rpc async_rpc;
    async_rpc.tag = 30; async_rpc.done = false; async_rpc.dead = false;
    async_rpc.be_reader = false; async_rpc.reply_len = 0; async_rpc.reply_buf = NULL;
    async_rpc.on_complete = test_handoff_async_recorder;   // async -> must be skipped
    rendez_init(&async_rpc.rendez);

    struct p9_rpc sync_rpc;
    sync_rpc.tag = 31; sync_rpc.done = false; sync_rpc.dead = false;
    sync_rpc.be_reader = false; sync_rpc.reply_len = 0; sync_rpc.reply_buf = NULL;
    sync_rpc.on_complete = NULL;                      // sync -> the handoff target
    rendez_init(&sync_rpc.rendez);

    spin_lock(&g_client.lock);
    g_client.inflight[30] = &async_rpc;
    g_client.inflight[31] = &sync_rpc;
    spin_unlock(&g_client.lock);

    p9_client_handoff_reader(&g_client);

    TEST_ASSERT(!async_rpc.be_reader, "async op NOT chosen as the elected reader");
    TEST_ASSERT(!g_handoff_async_fired, "async op's callback NOT invoked (it was skipped)");
    TEST_ASSERT(sync_rpc.be_reader, "sync op chosen as the elected reader");

    spin_lock(&g_client.lock);
    g_client.inflight[30] = NULL;
    g_client.inflight[31] = NULL;
    spin_unlock(&g_client.lock);

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-4 (LOOM.md §8.6): the deadline-aware reader pump. The loopback models a
// frame-boundary deadline (an armed deadline + an empty staged response returns
// -1 + recv_timed_out, instead of 0 = EOF), so the IDLE / PROGRESS / atomicity
// paths are driven deterministically without real time. `deadline_ns` is opaque
// to the loopback -- any non-zero value arms its knob.
// =============================================================================

static bool g_pump_async_completed;
static s32  g_pump_async_result;
static struct p9_rpc g_pump_rpc;

static void pump_async_on_complete(struct p9_rpc *rpc, int status,
                                   struct p9_dispatch_result *dr) {
    (void)rpc; (void)dr;
    g_pump_async_result    = (s32)status;
    g_pump_async_completed = true;
}

// The idle deadline lapses at a frame boundary (empty stream): the pump returns
// P9_PUMP_IDLE, the session stays alive + synced, and a subsequent op succeeds.
void test_9p_client_pump_deadline_idle(void) {
    drive_client_open(&g_client, &g_loopback);   // handshake drains the loopback

    const u64 deadline = 1;   // any non-zero value arms the loopback's knob
    int r = p9_client_reader_pump_once_deadline(&g_client, deadline);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_IDLE, "empty stream + armed deadline -> IDLE");
    TEST_ASSERT(!g_client.dead, "IDLE must NOT mark the session dead");
    TEST_ASSERT(p9_client_is_open(&g_client), "session still open after IDLE");

    // The stream stayed synced: a normal op still works (the IDLE consumed no
    // bytes, so the next reply is not mis-framed).
    int wrc = p9_client_walk_one(&g_client, 0, 31, (const u8 *)"f", 1, NULL);
    TEST_EXPECT_EQ(wrc, 0, "walk succeeds after IDLE (session reusable)");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// A reply already on the wire beats the deadline: the first recv returns bytes
// (never a timeout), so the pump demuxes the frame -> P9_PUMP_PROGRESS.
void test_9p_client_pump_deadline_data_ready_progresses(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 32, (const u8 *)"f", 1, NULL);   // bind fid 32

    g_pump_rpc.on_complete = pump_async_on_complete;
    g_pump_async_completed = false;
    g_pump_async_result    = 0x7fffffff;
    u32 fid = 32;
    int rc = p9_client_submit_async(&g_client, &g_pump_rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async stages an Rclunk on the wire");

    const u64 deadline = 1;
    int r = p9_client_reader_pump_once_deadline(&g_client, deadline);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_PROGRESS, "data ready -> PROGRESS (not IDLE)");
    TEST_ASSERT(g_pump_async_completed, "on_complete fired");
    TEST_EXPECT_EQ(g_pump_async_result, 0, "clunk success -> result 0");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Frame atomicity: with the frame delivered in sub-header chunks AND a deadline
// armed, the deadline is disarmed after the FIRST recv (one byte in hand = mid-
// frame), so aggregation completes and the whole frame demuxes -> PROGRESS. The
// deadline never fires mid-frame.
void test_9p_client_pump_deadline_chunked_frame_completes(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 33, (const u8 *)"f", 1, NULL);

    p9_loopback_set_chunk_size(&g_loopback, 3);   // < the 7-byte Rclunk frame

    g_pump_rpc.on_complete = pump_async_on_complete;
    g_pump_async_completed = false;
    u32 fid = 33;
    int rc = p9_client_submit_async(&g_client, &g_pump_rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async stages a chunked Rclunk");

    const u64 deadline = 1;
    int r = p9_client_reader_pump_once_deadline(&g_client, deadline);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_PROGRESS, "chunked frame under a deadline -> PROGRESS");
    TEST_ASSERT(g_pump_async_completed, "on_complete fired for the aggregated frame");

    p9_loopback_set_chunk_size(&g_loopback, 0);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Another thread already holds the reader role: the pump defers (P9_PUMP_BUSY)
// without touching the stream. White-box: set reader_active directly.
void test_9p_client_pump_deadline_busy_when_reader_active(void) {
    drive_client_open(&g_client, &g_loopback);

    g_client.reader_active = true;     // simulate a concurrent elected reader
    const u64 deadline = 1;
    int r = p9_client_reader_pump_once_deadline(&g_client, deadline);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_BUSY, "reader already active -> BUSY (no-op)");
    TEST_ASSERT(!g_client.dead, "BUSY must not mark the session dead");
    g_client.reader_active = false;    // release so destroy is clean

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-3 engine-driven tests: the full SYS_LOOM_ENTER path against the loopback
// 9P client -- register a dev9p Spoor in a Loom, stage an FSYNC SQE, loom_enter
// submits (Tfsync via the submit-time pin) + pumps the reply (Rfsync) + posts a
// CQE. Plus the I-30 rights gate (a read-only handle denies fsync) and the #898
// quiesce (a loom torn down with an op in flight abandons it cleanly).
// =============================================================================

// Stage one SQE into a Loom's ring (kernel direct map), identity SQ-index
// indirection (ring slot `slot` -> SQE `slot`). Mirrors test_loom.c's helper.
static void cl_stage_sqe(struct Loom *l, u32 slot, u8 opcode, u32 handle_idx,
                         u32 len, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode     = opcode;
    s->handle_idx = handle_idx;
    s->len        = len;
    s->user_data  = user_data;
    sqa[slot] = slot;
}

// FSYNC end-to-end: the SQE dispatches a Tfsync against the registered dev9p
// Spoor's (client, fid); the elected reader pumps the Rfsync; on_complete posts
// a success CQE. Exercises Consume + the submit-time pin + Dispatch +
// ReplyArrives + PostCqe + the reap.
void test_9p_client_loom_fsync_e2e(void) {
    drive_client_open(&g_client, &g_loopback);   // handshake; root_fid 0 bound, OPEN

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);   // root Spoor (fid 0)
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor (adopts ref)");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe(l, 0, LOOM_OP_FSYNC, /*handle_idx=*/0, /*len=datasync*/0,
                 0xF00DCAFE12345678ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, /*to_submit=*/1, /*min_complete=*/1, /*flags=*/0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE posted (fsync completed)");
    TEST_EXPECT_EQ(cqes[0].user_data, 0xF00DCAFE12345678ULL, "CQE user_data echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "fsync success -> result 0");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)1, "sq_head advanced + mirrored");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op completed + reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "container reclaimed by loom_reap_terminal");

    loom_unref(l);                  // clunks the registered spoor (fid_owned=false -> client untouched)
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// I-30 submit-time rights pin: an FSYNC against a registered handle whose rights
// snapshot lacks RIGHT_WRITE is denied at submit (-EACCES CQE) and NEVER
// dispatched -- the op does not go in flight, so the gate cannot be bypassed by
// a later re-resolve at completion.
void test_9p_client_loom_rights_deny(void) {
    drive_client_open(&g_client, &g_loopback);

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ;       // NO RIGHT_WRITE -> fsync denied
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register read-only handle");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe(l, 0, LOOM_OP_FSYNC, 0, 0, 0xDEADBEEFu);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one (error) CQE");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_ACCES),
                   "read-only handle -> fsync -EACCES (I-30 rights pin)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "denied at submit -> never dispatched");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// #898 quiesce: a Loom torn down with an async op in flight must abandon it --
// Tflush on the client (clearing inflight[tag] so a late reply is discarded
// ownerless), release the submit-time pin, and free the container -- with no
// hang, no leak, and no use-after-free. Submit WITHOUT pumping (the reply is
// staged but not demuxed), then loom_unref drives loom_free's quiesce.
void test_9p_client_loom_quiesce_abandons_inflight(void) {
    drive_client_open(&g_client, &g_loopback);

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    cl_stage_sqe(l, 0, LOOM_OP_FSYNC, 0, 0, 0x5151515151515151ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    // Submit-only (min_complete 0, NONBLOCK): the Tfsync is sent + the op is in
    // flight, but the reply is NOT demuxed (no pump) -- so it sits non-terminal.
    int n = loom_enter(l, 1, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 1, "one SQE submitted");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)1, "op is in flight (not pumped)");
    TEST_ASSERT(l->inflight_ops != NULL, "container linked");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)0, "no CQE (op not completed)");

    u64 freed0     = spoor_total_freed();
    u64 destroyed0 = loom_total_destroyed();

    // Tear the ring down with the op in flight (#898). loom_free quiesces: the
    // abandon clears inflight[tag] + Tflushes, the pin is clunked, the container
    // freed. The dev9p root spoor (reg ref + the op's pin ref = 2) is fully
    // released; the loom is destroyed exactly once. No hang / leak / UAF.
    loom_unref(l);
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1, "loom freed once");
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, (u64)1, "dev9p spoor freed (both refs released)");

    // A late reply (the original Rfsync was staged, then overwritten by the
    // abandon's Rflush) now arrives. The abandon cleared inflight[tag], so demux
    // discards it ownerless -- it must NOT touch the freed container. No UAF.
    int pumped = p9_client_reader_pump_once(&g_client);
    TEST_ASSERT(pumped == 1 || pumped == -P9_E_IO, "late reply drained ownerless (no UAF)");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Stage one MULTISHOT FSYNC SQE: LOOM_SQE_MULTISHOT + the synthetic shot_limit
// (total CQEs in the stream) carried in sqe->offset (FSYNC ignores the offset).
static void cl_stage_multishot_fsync(struct Loom *l, u32 slot, u32 handle_idx,
                                     u64 shot_limit, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode     = LOOM_OP_FSYNC;
    s->flags      = LOOM_SQE_MULTISHOT;
    s->handle_idx = handle_idx;
    s->offset     = shot_limit;
    s->user_data  = user_data;
    sqa[slot] = slot;
}

// Multishot stream (specs/loom_multishot.tla): ONE LOOM_SQE_MULTISHOT FSYNC SQE
// produces a STREAM of CQEs -- (N-1) LOOM_CQE_MORE shots that each RE-ARM the op
// + ONE MORE-clear terminal -- driven by repeated Tfsync->Rfsync round-trips in a
// SINGLE loom_enter. The submit-time pin (the dev9p spoor) is held across ALL
// shots + released exactly once at the terminal (ObjPinnedAcrossShots;
// ExactlyOneTerminal; TerminalEndsStream).
void test_9p_client_loom_multishot_stream(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_multishot_fsync(l, 0, /*handle_idx=*/0, /*shot_limit=*/3,
                             0xA5A5000012345678ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    // min_complete=3: drive 2 MORE shots + the terminal in one enter.
    int n = loom_enter(l, /*to_submit=*/1, /*min_complete=*/3, /*flags=*/0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (the stream is one op)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "3 CQEs posted (2 MORE + 1 terminal)");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) != 0, "shot 0 sets LOOM_CQE_MORE");
    TEST_ASSERT((cqes[1].flags & LOOM_CQE_MORE) != 0, "shot 1 sets LOOM_CQE_MORE");
    TEST_ASSERT((cqes[2].flags & LOOM_CQE_MORE) == 0, "terminal clears LOOM_CQE_MORE");
    for (u32 i = 0; i < 3; i++) {
        TEST_EXPECT_EQ(cqes[i].user_data, 0xA5A5000012345678ULL, "every CQE echoes user_data");
        TEST_EXPECT_EQ((u64)(s64)cqes[i].result, (u64)0, "every shot succeeds (result 0)");
    }
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "stream terminal -> nothing in flight");
    TEST_ASSERT(l->inflight_ops == NULL, "terminal op reaped (one container, whole stream)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no shot dropped (CqNeverOverfull)");

    u64 freed0 = spoor_total_freed();
    loom_unref(l);                  // releases the reg ref (the pin was released at the terminal)
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, (u64)1,
                   "dev9p spoor freed once (pin held across all shots, released exactly once)");
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Multishot back-pressure (CqNeverOverfull, NOT BUGGY_SHOT_LOST_ON_FULL): a MORE
// shot is HELD -- never dropped -- when the CQ is full, and the stream RESUMES
// when userspace reaps a slot + re-enters. cq_entries=2, shot_limit=4 (3 MORE + 1
// terminal): the first enter fills the 2-slot CQ with 2 MORE shots then the op
// stays rearm-pending (held, not reaped); after reaping both, the second enter
// re-arms + drains the rest. `overflow` stays 0 throughout (the shot was held, not
// dropped into a full CQ).
void test_9p_client_loom_multishot_backpressure(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(2, 2);   // cq_entries = 2 (smallest exercising the hold)
    TEST_ASSERT(l != NULL, "loom_create(2,2)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_multishot_fsync(l, 0, 0, /*shot_limit=*/4, 0xBACE000087654321ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    // Enter 1: fills the 2-slot CQ with 2 MORE shots, then the 3rd shot cannot
    // admit -- the op HOLDS (rearm-pending, not terminal, not reaped).
    int n = loom_enter(l, 1, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)2, "CQ filled with 2 MORE shots");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) != 0, "shot 0 MORE");
    TEST_ASSERT((cqes[1].flags & LOOM_CQE_MORE) != 0, "shot 1 MORE");
    TEST_ASSERT(l->inflight_ops != NULL, "op HELD (rearm-pending), not reaped");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no request in flight (held for CQ room)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no shot dropped -- held, not overflowed");

    // Userspace reaps both CQEs (advance cq_head): the CQ now has room.
    __atomic_store_n(&h->cq_head, 2u, __ATOMIC_RELEASE);

    // Enter 2: re-arms the held op + drives the remaining MORE shot + the terminal.
    // The CQ wraps (cq_entries=2): tail 2->slot 0 (shot 2, MORE), tail 3->slot 1
    // (terminal, MORE-clear) -- both over already-reaped slots.
    n = loom_enter(l, 0, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ(n, 0, "no new SQE (the resume re-arms the held op)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "stream resumed: shot 2 + terminal posted");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) != 0, "shot 2 MORE (slot 0 wrapped)");
    TEST_ASSERT((cqes[1].flags & LOOM_CQE_MORE) == 0, "terminal clears MORE (slot 1 wrapped)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "stream done");
    TEST_ASSERT(l->inflight_ops == NULL, "terminal op reaped");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "overflow still 0 -- back-pressure, never dropped");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}
