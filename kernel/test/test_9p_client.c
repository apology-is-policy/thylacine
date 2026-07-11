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
#include <thylacine/9p_transport_mq.h>   // Loom-6c multi-in-flight queueing transport
#include <thylacine/9p_wire.h>
#include <thylacine/burrow.h>     // Loom-6 white-box registered-buffer install
#include <thylacine/dev9p.h>
#include <thylacine/weft.h>       // Weft-6c: weft_binding_alloc + the zero-copy drive
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/page.h>       // pa_to_kva / page_to_pa (the buffer direct map)
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
void test_9p_client_weft(void);
void test_9p_client_weftio(void);
void test_9p_client_mkdir(void);
void test_9p_client_unlinkat(void);
void test_9p_client_readlink(void);
void test_9p_client_rlerror_propagates_to_negative_errno(void);
void test_9p_client_rlerror_hostile_ecode_bounded(void);
void test_9p_client_op_before_handshake_returns_ebusy(void);
void test_9p_client_lock_released_between_ops(void);
void test_9p_client_async_op_posts_cqe(void);
void test_9p_client_async_session_death_posts_error_cqe(void);
void test_9p_client_async_peer_gone_posts_nodev_cqe(void);
void test_9p_client_async_mark_devgone_posts_nodev_cqe(void);
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
void test_9p_client_loom_read_e2e(void);
void test_9p_client_loom_write_e2e(void);
void test_9p_client_loom_rw_rejects(void);
void test_9p_client_loom_readdir_e2e(void);
void test_9p_client_loom_readlink_e2e(void);
void test_9p_client_loom_getattr_e2e(void);
void test_9p_client_loom_statfs_e2e(void);
void test_9p_client_loom_metaread_rejects(void);
void test_9p_client_loom_mkdir_e2e(void);
void test_9p_client_loom_setattr_e2e(void);
void test_9p_client_loom_renameat_e2e(void);
void test_9p_client_loom_mutation_rejects(void);

// File-scope buffers (kernel test stack is 16 KiB — client struct is
// ~4 KiB; multiple in one frame is fine but file-scope is cleaner).
static u8 g_recv_buf[4096];
static u8 g_loopback_resp[4096];

// Reusable responder that handles every Tmsg type with a sensible
// canned response. Tests choose which fid path / qid the server
// returns by reading the request opcode + tag. Non-static: the
// SrvConn-vehicle device-gone tests (test_9p_srvconn_transport.c)
// pre-stage handshake replies through it -- the single source of
// truth for the canonical 9P2000.L reply byte layouts.
// Staged by the walkgetattr test: the responder answers one element
// FEWER than requested (a partial walk).
static bool g_wga_partial = false;

int canonical_responder(void *ctx, const u8 *req, size_t req_len,
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
    if (type == P9_TWALKGETATTR) {
        // POUNCE: echo the REQUESTED nwname back as full-walk elements
        // (or one fewer when the partial-walk flag is staged), each with
        // distinctive per-index attrs so client-side extraction is
        // assertable. Bytes hand-written (independent of the builders).
        if (req_len < P9_HDR_LEN + 18) return -1;
        u16 nwname = (u16)(req[P9_HDR_LEN + 16] |
                           ((u16)req[P9_HDR_LEN + 17] << 8));
        u16 n = (g_wga_partial && nwname > 0) ? (u16)(nwname - 1) : nwname;
        size_t total = P9_HDR_LEN + 2 + (size_t)n * P9_WGA_BODY_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWALKGETATTR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(n & 0xff); resp[8] = (u8)((n >> 8) & 0xff);
        for (u16 i = 0; i < n; i++) {
            u8 *el = resp + 9 + (size_t)i * P9_WGA_BODY_LEN;
            for (u32 b = 0; b < P9_WGA_BODY_LEN; b++) el[b] = 0;
            el[0] = 0xFF; el[1] = 0x3F;                  // valid = ALL
            el[8] = (i + 1 == n) ? P9_QTFILE : P9_QTDIR; // qid.type
            el[13] = (u8)(200 + i);                      // qid.path
            if (i + 1 == n) { el[21] = 0xA4; el[22] = 0x81; }  // 0100644
            else            { el[21] = 0xED; el[22] = 0x41; }  // 0040755
            el[25] = 7;                                  // uid
            el[29] = 8;                                  // gid
            el[33] = 1;                                  // nlink
            el[49] = (u8)(100 + i);                      // size
        }
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
    if (type == P9_TWEFT) {
        // Canned Rweft: share_id 0x1122334455667788, ring_size 64 KiB,
        // ring_entries 256. Body = [share_id u64][ring_size u32][entries u32].
        // Bytes hand-written (independent of p9_build_rweft, so a builder bug
        // can't mask a dispatch/copy-out bug at the client-composition layer).
        size_t total = P9_HDR_LEN + 8 + 4 + 4;   // 23
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWEFT;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        // share_id = 0x1122334455667788 (little-endian)
        resp[7]  = 0x88; resp[8]  = 0x77; resp[9]  = 0x66; resp[10] = 0x55;
        resp[11] = 0x44; resp[12] = 0x33; resp[13] = 0x22; resp[14] = 0x11;
        // ring_size = 0x00010000 (64 KiB)
        resp[15] = 0x00; resp[16] = 0x00; resp[17] = 0x01; resp[18] = 0x00;
        // ring_entries = 256 = 0x00000100
        resp[19] = 0x00; resp[20] = 0x01; resp[21] = 0x00; resp[22] = 0x00;
        return (int)total;
    }
    if (type == P9_TWEFTIO) {
        // Canned Rweftio: count = 0x00001000 (4096). Body = [count u32].
        // Hand-written (independent of p9_build_rweftio) so a builder bug can't
        // mask a dispatch / copy-out bug at the client-composition layer.
        size_t total = P9_HDR_LEN + 4;   // 11
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RWEFTIO;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        // count = 0x00001000 (little-endian)
        resp[7] = 0x00; resp[8] = 0x10; resp[9] = 0x00; resp[10] = 0x00;
        return (int)total;
    }
    return -1;
}

// Responder that always returns Rlerror. The ecode is file-scope so the
// hostile-ecode test can stage out-of-range values; default 2 (ENOENT).
static u32 g_rlerror_ecode = 2;

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
    // Anything else: Rlerror with ecode = g_rlerror_ecode.
    size_t total = P9_HDR_LEN + 4;
    if (resp_cap < total) return -1;
    resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
    resp[4] = P9_RLERROR;
    resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
    resp[7]  = (u8)(g_rlerror_ecode & 0xff);
    resp[8]  = (u8)((g_rlerror_ecode >> 8) & 0xff);
    resp[9]  = (u8)((g_rlerror_ecode >> 16) & 0xff);
    resp[10] = (u8)((g_rlerror_ecode >> 24) & 0xff);
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

void test_9p_client_walkgetattr(void) {
    drive_client_open(&g_client, &g_loopback);

    const u8 *names[2];
    size_t lens[2];
    names[0] = (const u8 *)"a"; lens[0] = 1;
    names[1] = (const u8 *)"f"; lens[1] = 1;
    u16 nwqid;
    struct p9_qid  qids[P9_MAX_WALK];
    struct p9_attr attrs[2];

    // Full walk with a real newfid: binds + per-component attrs extract.
    int rc = p9_client_walkgetattr(&g_client, /*src=*/0, /*new=*/30,
                                   P9_GETATTR_ALL, 2, names, lens,
                                   &nwqid, qids, attrs);
    TEST_EXPECT_EQ(rc, 0,                              "walkgetattr(0->30) ok");
    TEST_EXPECT_EQ((u64)nwqid, (u64)2,                 "2 elements");
    TEST_EXPECT_EQ(qids[0].path, (u64)200,             "qid0.path 200");
    TEST_EXPECT_EQ(qids[1].path, (u64)201,             "qid1.path 201");
    TEST_EXPECT_EQ((u64)attrs[0].mode, (u64)0x41ED,    "attr0 dir mode");
    TEST_EXPECT_EQ((u64)attrs[1].mode, (u64)0x81A4,    "attr1 file mode");
    TEST_EXPECT_EQ((u64)attrs[1].uid, (u64)7,          "attr1 uid");
    TEST_EXPECT_EQ(attrs[1].size, (u64)101,            "attr1 size");
    TEST_EXPECT_EQ(attrs[0].qid.path, (u64)200,        "attr0 embedded qid");
    rc = p9_client_clunk(&g_client, 30);
    TEST_EXPECT_EQ(rc, 0,                              "bound newfid clunks ok");

    // NOFID query: attrs return; the session binds NOTHING.
    size_t fids_before = p9_session_n_bound_fids(&g_client.session);
    rc = p9_client_walkgetattr(&g_client, 0, P9_NOFID, P9_GETATTR_ALL,
                               2, names, lens, &nwqid, qids, attrs);
    TEST_EXPECT_EQ(rc, 0,                              "NOFID query ok");
    TEST_EXPECT_EQ((u64)nwqid, (u64)2,                 "query: 2 elements");
    TEST_EXPECT_EQ((u64)attrs[0].gid, (u64)8,          "query attr gid");
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session),
                   (u64)fids_before,                   "query bound NOTHING");

    // Partial walk with a real newfid: prefix attrs; newfid NOT bound.
    g_wga_partial = true;
    rc = p9_client_walkgetattr(&g_client, 0, 31, P9_GETATTR_ALL,
                               2, names, lens, &nwqid, qids, attrs);
    g_wga_partial = false;
    TEST_EXPECT_EQ(rc, 0,                              "partial walkgetattr ok");
    TEST_EXPECT_EQ((u64)nwqid, (u64)1,                 "partial: 1 element");
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session),
                   (u64)fids_before,                   "partial bound NOTHING");
    rc = p9_client_clunk(&g_client, 31);
    TEST_ASSERT(rc != 0,               "unbound partial newfid cannot clunk");

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

// CF-3 A regression: a write whose count exceeds the negotiated msize's
// Twrite payload must be CLAMPED to a short write (the POSIX contract;
// callers loop), never fail the frame build. Pre-clamp this returned
// -P9_E_IO for every over-payload write -- the bench cascade: the go
// compiler's bulk object writes EIO'd, no cache puts landed, the warm
// build ran cold. msize here is the negotiated 8192, so the payload max
// is 8192 - 23 (hdr 7 + fid 4 + offset 8 + count 4) = 8169.
void test_9p_client_bulk_write_clamps_short(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 14, (const u8 *)"f", 1, NULL);

    static u8 big[16000];
    for (u32 i = 0; i < sizeof(big); i++) big[i] = (u8)(i & 0xFF);
    u32 nmsize = g_client.session.negotiated_msize;
    TEST_ASSERT(nmsize > 0 && nmsize <= 8192, "loopback msize sanity");
    u64 wmax = (u64)nmsize - 23u;

    u32 accepted = 0;
    int rc = p9_client_write(&g_client, 14, 0,
                               (u32)sizeof(big), big, &accepted);
    TEST_EXPECT_EQ(rc, 0,               "over-payload write must not EIO");
    TEST_EXPECT_EQ((u64)accepted, wmax, "accepted = the msize payload max");

    // The read-side twin clamp: an over-payload count is clamped before the
    // Tread goes out (observable only as rc==0 here -- the loopback file is
    // 5 bytes; the pre-clamp count would have been legal on the wire anyway
    // since Tread carries no payload, but the clamp keeps the REPLY bound
    // inside the negotiated msize by construction).
    static u8 rbuf[16000];
    u32 n = 0;
    rc = p9_client_read(&g_client, 14, 0, (u32)sizeof(rbuf), rbuf, &n);
    TEST_EXPECT_EQ(rc, 0, "over-payload read count must not error");

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

// Weft-6a-1: p9_client_weft composition -- send_weft -> client_run ->
// dispatch (Rweft) -> copy the geom out. Fid 20 stands in for an opened
// /net data fid; the canned Rweft carries a known share_id + geometry.
void test_9p_client_weft(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);

    struct p9_weft_geom geom;
    int rc = p9_client_weft(&g_client, 20, &geom);
    TEST_EXPECT_EQ(rc, 0,                                       "weft ok");
    TEST_EXPECT_EQ(geom.share_id, (u64)0x1122334455667788ULL,   "share_id round-trip");
    TEST_EXPECT_EQ((u64)geom.ring_size, (u64)0x00010000,        "ring_size round-trip");
    TEST_EXPECT_EQ((u64)geom.ring_entries, (u64)256,            "ring_entries round-trip");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Weft-6b-2a: p9_client_weftio composition -- send_weftio -> client_run ->
// dispatch (Rweftio) -> copy the count out. Fid 20 stands in for an opened
// /net data fid; the canned Rweftio carries a known moved-byte count.
void test_9p_client_weftio(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);

    u32 count = 0;
    int rc = p9_client_weftio(&g_client, 20, /*off=*/0x100u, /*len=*/0x800u,
                              WEFT_DIR_WRITE, &count);
    TEST_EXPECT_EQ(rc, 0,                        "weftio ok");
    TEST_EXPECT_EQ((u64)count, (u64)0x00001000u, "weftio count round-trip");

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

// A hostile/buggy server controls the Rlerror ecode wire field. The
// client bounds it before negating (9p_client.c map_error): ecode 0 (an
// error reply must carry a nonzero errno) and anything past the 4095
// errno window collapse to -P9_E_IO — without the bound, -(int)ecode on
// 0x80000000 is signed-overflow UB (a UBSan kernel halt reachable by any
// Rlerror). Regression for the bound (RW-10 ledger I-14 test gap).
void test_9p_client_rlerror_hostile_ecode_bounded(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                rlerror_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (hostile rlerror)");

    rc = p9_client_init(&g_client, 0, 8192,
                         p9_loopback_ops_for(&g_loopback),
                         g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");

    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    rc = p9_client_handshake(&g_client, uname, sizeof(uname),
                               aname, sizeof(aname), 0);
    TEST_EXPECT_EQ(rc, 0, "handshake ok");

    // ecode = 0: collapses to -EIO, never 0 ("success" from an error).
    g_rlerror_ecode = 0;
    rc = p9_client_walk_one(&g_client, 0, 5, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -P9_E_IO, "ecode 0 collapses to -EIO");

    // ecode = 0x80000000: -(int)ecode would be signed-overflow UB.
    g_rlerror_ecode = 0x80000000u;
    rc = p9_client_walk_one(&g_client, 0, 6, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -P9_E_IO, "ecode 2^31 collapses to -EIO");

    // ecode = 4096: one past the pouch [-4095,-2] passthrough window.
    g_rlerror_ecode = 4096;
    rc = p9_client_walk_one(&g_client, 0, 7, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -P9_E_IO, "ecode 4096 collapses to -EIO");

    // Control: 4095 (in-window) passes through as -4095.
    g_rlerror_ecode = 4095;
    rc = p9_client_walk_one(&g_client, 0, 8, (const u8 *)"x", 1, NULL);
    TEST_EXPECT_EQ(rc, -4095, "ecode 4095 passes through");

    g_rlerror_ecode = 2;
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

    // The TRANSPORT-ERROR death leg (MENAGERIE.md section 10): destroy closes the
    // loopback, so the next recv returns -1 (an error, NOT a clean EOF) -> the
    // reader marks the session dead with the TRANSPORT reason -> the in-flight
    // async op completes with the generic -EIO. (The device-gone leg, a clean
    // peer-gone EOF, is the two tests below; they yield -ENODEV.)
    p9_loopback_destroy(&g_loopback);
    int pumped = p9_client_reader_pump_once(&g_client);
    TEST_EXPECT_EQ(pumped, -P9_E_IO, "pump sees the dead transport");
    TEST_ASSERT(g_async_op.completed, "async op completed on session death");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)(-P9_E_IO),
                    "transport-error CQE result = -EIO (not device-gone)");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one (error) CQE posted");

    p9_client_destroy(&g_client);   // loopback already destroyed
    loom_unref(l);
}

// Device-gone leg 1 (MENAGERIE.md section 10): a PEER-GONE EOF -- the server /
// driver endpoint closed cleanly (recv 0), the automatic path a DeviceRemoved
// drives -- completes the in-flight async op with the device-gone -ENODEV
// terminal CQE, distinct from the transport -EIO above. force_eof drops the
// staged reply WITHOUT closing the transport, so the next recv returns 0.
void test_9p_client_async_peer_gone_posts_nodev_cqe(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 23, (const u8 *)"f", 1, NULL);

    g_async_op.loom        = l;
    g_async_op.user_data   = 0xD00DFEED;
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    g_async_op.rpc.on_complete = test_async_on_complete;

    u32 fid = 23;
    int rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async succeeds (op in flight)");
    TEST_ASSERT(!g_async_op.completed, "not yet completed");

    // The server endpoint vanishes cleanly: drop the staged reply so the pump's
    // recv returns 0 (a clean EOF = peer gone), NOT -1 (an error). The reader
    // classifies this device-gone -> the op gets a -ENODEV CQE.
    p9_loopback_force_eof(&g_loopback);
    int pumped = p9_client_reader_pump_once(&g_client);
    TEST_EXPECT_EQ(pumped, -P9_E_IO, "pump returns DEAD (a control signal)");
    TEST_ASSERT(g_async_op.completed, "async op completed on the peer-gone EOF");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)(-P9_E_NODEV),
                    "device-gone CQE result = -ENODEV (not -EIO)");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one (device-gone) CQE posted");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-P9_E_NODEV),
                    "the posted CQE carries -ENODEV");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
    loom_unref(l);
}

// Device-gone leg 2 (MENAGERIE.md section 10): the EXPLICIT entry point. A holder
// of the client (a device-teardown / warden-removal hook) calls
// p9_client_mark_devgone to proactively fail every in-flight async op with the
// device-gone -ENODEV terminal -- no transport interaction, fully deterministic.
void test_9p_client_async_mark_devgone_posts_nodev_cqe(void) {
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 24, (const u8 *)"f", 1, NULL);

    g_async_op.loom        = l;
    g_async_op.user_data   = 0xFEEDFACE;
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    g_async_op.rpc.on_complete = test_async_on_complete;

    u32 fid = 24;
    int rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async succeeds (op in flight)");
    TEST_ASSERT(!g_async_op.completed, "not yet completed");

    // The explicit device-gone mark: complete the in-flight op NOW with -ENODEV.
    p9_client_mark_devgone(&g_client);
    TEST_ASSERT(g_async_op.completed, "mark_devgone completed the in-flight op");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)(-P9_E_NODEV),
                    "explicit mark_devgone -> -ENODEV CQE");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one CQE posted");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-P9_E_NODEV),
                    "the posted CQE carries -ENODEV");

    // Idempotent: a second mark on the already-dead session is a no-op (the
    // in-flight slot was cleared), so no second CQE is posted.
    p9_client_mark_devgone(&g_client);
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "mark_devgone idempotent -- still one CQE");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
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

// =============================================================================
// Loom-5b LINK/DRAIN chain (specs/loom_order.tla): an SQE with LINK/DRAIN (or any
// op consumed while the chain is non-empty) is HELD until its ordering gates open.
// These tests drive the full loom_enter path against the loopback 9P client:
// FSYNC against a write handle is async (a Rfsync drives completion); FSYNC
// against a read-only handle fails inline (-EACCES) -- the deterministic head-
// failure for the cancel-cascade; NOP completes inline. CQ order (cq index +
// user_data) witnesses the admission order.
// =============================================================================

// Stage one SQE with explicit flags (LINK / DRAIN). Identity SQ-index indirection.
static void cl_stage_sqe_flags(struct Loom *l, u32 slot, u8 opcode, u8 flags,
                               u32 handle_idx, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode     = opcode;
    s->flags      = flags;
    s->handle_idx = handle_idx;
    s->user_data  = user_data;
    sqa[slot] = slot;
}

// LINK cancel-cascade (LinkOrdered + EveryDoneOpPosted + NoOrphanCancel): a chain
// [FSYNC(read-only, LINK)][NOP] -- the head fails inline (-EACCES), so the linked
// NOP successor is CANCELLED with exactly ONE -ECANCELED CQE and is NEVER
// dispatched. The cancel is not silently dropped (EveryDoneOpPosted).
void test_9p_client_loom_link_cancel_cascade(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ;            // NO RIGHT_WRITE -> the head FSYNC fails inline
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register read-only handle");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe_flags(l, 0, LOOM_OP_FSYNC, LOOM_SQE_LINK, /*handle=*/0, 0xC0DE0001u);
    cl_stage_sqe_flags(l, 1, LOOM_OP_NOP, 0, 0, 0xC0DE0002u);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    int n = loom_enter(l, /*to_submit=*/2, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ(n, 2, "two SQEs consumed (both routed to the chain)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)2, "two CQEs (head fail + successor cancel)");
    TEST_EXPECT_EQ(cqes[0].user_data, 0xC0DE0001u, "CQE0 = the head FSYNC");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_ACCES),
                   "head FSYNC denied (-EACCES), DONE_FAIL");
    TEST_EXPECT_EQ(cqes[1].user_data, 0xC0DE0002u, "CQE1 = the cancelled successor");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_CANCELED),
                   "linked successor CANCELLED (-ECANCELED), exactly one CQE");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "nothing in flight (head failed inline)");
    TEST_ASSERT(l->inflight_ops == NULL, "no async op (cascade is all inline)");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// LINK success ordering (LinkOrdered): a chain [FSYNC(write, LINK)][NOP] -- the
// linked NOP runs ONLY after the FSYNC's Rfsync completes. Witness: the FSYNC CQE
// (async) is posted BEFORE the NOP CQE; if the link gate were dropped the NOP
// (inline) would post first.
void test_9p_client_loom_link_success_ordering(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write handle");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe_flags(l, 0, LOOM_OP_FSYNC, LOOM_SQE_LINK, /*handle=*/0, 0x11110001u);
    cl_stage_sqe_flags(l, 1, LOOM_OP_NOP, 0, 0, 0x11110002u);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 2, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ(n, 2, "two SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)2, "two CQEs");
    TEST_EXPECT_EQ(cqes[0].user_data, 0x11110001u, "CQE0 = FSYNC (the predecessor, async)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "FSYNC succeeded");
    TEST_EXPECT_EQ(cqes[1].user_data, 0x11110002u, "CQE1 = NOP (admitted only AFTER FSYNC done)");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)0, "NOP succeeded");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all done + reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "async container reclaimed");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// DRAIN barrier (DrainOrdered): a chain [FSYNC A][NOP DRAIN B][NOP C] -- A is a
// FAST async op (no flags, dispatched before the chain starts); the DRAIN op B
// must wait for A's async completion (async_inflight -> 0, the load-bearing
// drain-self gate that catches prior non-chain async ops) before it admits; the
// post-drain op C waits for B. Witness: B's CQE is at index 1 (AFTER A, not
// inline-first), C's at index 2. (One prior async op, not two: the loopback test
// transport is strictly single-in-flight -- it refuses a send while a prior
// response is unread -- so a second concurrent async op would fail the loopback's
// own discipline, not the drain gate. The async_inflight==0 gate is identical for
// one or many prior ops; the multi-op barrier is covered by loom_order.tla.)
void test_9p_client_loom_drain_barrier(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write handle");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe_flags(l, 0, LOOM_OP_FSYNC, 0, /*handle=*/0, 0xAAAA0001u);  // A (fast async)
    cl_stage_sqe_flags(l, 1, LOOM_OP_NOP, LOOM_SQE_DRAIN, 0, 0xBBBB0002u);  // B (drain barrier)
    cl_stage_sqe_flags(l, 2, LOOM_OP_NOP, 0, 0, 0xCCCC0003u);               // C (post-drain)
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 3, /*min_complete=*/3, 0);
    TEST_EXPECT_EQ(n, 3, "three SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "three CQEs");
    // A posts first (its async Rfsync), then the barrier B, then C.
    TEST_EXPECT_EQ(cqes[0].user_data, 0xAAAA0001u, "CQE0 = A (the prior async op)");
    TEST_EXPECT_EQ(cqes[1].user_data, 0xBBBB0002u,
                   "CQE1 = DRAIN B -- AFTER A (not inline-first; the barrier held)");
    TEST_EXPECT_EQ(cqes[2].user_data, 0xCCCC0003u, "CQE2 = C -- after the drain");
    for (u32 i = 0; i < 3; i++)
        TEST_EXPECT_EQ((u64)(s64)cqes[i].result, (u64)0, "every op succeeded");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all done + reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "async container reclaimed");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Independent op admits past a HELD linked op (the loom_order.tla head->tail
// continue-not-break admission): a chain [FSYNC A (LINK)][NOP B][NOP C] -- B is
// A's linked successor (held until A's Rfsync); C is INDEPENDENT (B does not link
// to it), so C admits IMMEDIATELY, out of order, while B is still held. Witness:
// C's CQE is index 0 (before A and B); then A, then B.
void test_9p_client_loom_independent_past_held(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write handle");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_sqe_flags(l, 0, LOOM_OP_FSYNC, LOOM_SQE_LINK, /*handle=*/0, 0xA0000001u);  // A (links to B)
    cl_stage_sqe_flags(l, 1, LOOM_OP_NOP, 0, 0, 0xB0000002u);   // B (A's held successor)
    cl_stage_sqe_flags(l, 2, LOOM_OP_NOP, 0, 0, 0xC0000003u);   // C (independent of B)
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 3, /*min_complete=*/3, 0);
    TEST_EXPECT_EQ(n, 3, "three SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "three CQEs");
    TEST_EXPECT_EQ(cqes[0].user_data, 0xC0000003u,
                   "CQE0 = C -- the independent op admitted PAST the held B");
    TEST_EXPECT_EQ(cqes[1].user_data, 0xA0000001u, "CQE1 = A (the linked predecessor)");
    TEST_EXPECT_EQ(cqes[2].user_data, 0xB0000002u, "CQE2 = B (admitted only after A done)");
    for (u32 i = 0; i < 3; i++)
        TEST_EXPECT_EQ((u64)(s64)cqes[i].result, (u64)0, "every op succeeded");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all done + reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "async container reclaimed");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// DRAIN waits for a rearm-pending FAST multishot (audit F1 regression, DrainOrdered):
// a FAST (chain-empty) MULTISHOT stream that is BACK-PRESSURED (rearm-pending,
// async_inflight==0 but its stream not done) must still hold a later DRAIN. The
// drain is submitted in a NONBLOCK enter whose submit-phase loom_admit_chain has no
// preceding loom_rearm_pending -- so without the rearm_pending term in the drain
// gate, the drain would admit early (cq_tail bumps to 3 here). With the fix it
// HOLDS (cq_tail stays 2) until the multishot terminates.
void test_9p_client_loom_drain_waits_for_rearm_pending(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(2, 2);   // cq=2 forces the multishot to back-pressure
    TEST_ASSERT(l != NULL, "loom_create(2,2)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // FAST multishot (no LINK/DRAIN, chain empty): shot_limit=3 (2 MORE + terminal).
    cl_stage_multishot_fsync(l, 0, /*handle_idx=*/0, /*shot_limit=*/3, 0x3EA10001u);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
    int n = loom_enter(l, 1, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ(n, 1, "multishot SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)2, "CQ filled with 2 MORE shots");
    TEST_EXPECT_EQ((u64)l->rearm_pending, (u64)1, "multishot HELD rearm-pending (back-pressured)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no request in flight (rearm-pending)");

    // Reap the 2 shots so the CQ has room (else the drain's own CQ gate would mask
    // the bug). The multishot stays rearm-pending.
    __atomic_store_n(&h->cq_head, 2u, __ATOMIC_RELEASE);

    // Submit a DRAIN, NONBLOCK: the submit-phase admit runs but does NOT wait. The
    // drain must HOLD (the rearm-pending multishot is a prior FAST op, not done).
    cl_stage_sqe_flags(l, 1, LOOM_OP_NOP, LOOM_SQE_DRAIN, 0, 0x3EA10002u);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);
    n = loom_enter(l, 1, /*min_complete=*/0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 1, "DRAIN SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)2,
                   "DRAIN HELD -- did NOT admit early past the rearm-pending multishot (F1)");
    TEST_ASSERT(l->chain != NULL, "DRAIN still in the chain (held)");

    // Drive to completion: the multishot re-arms + terminates, THEN the drain admits.
    n = loom_enter(l, 0, /*min_complete=*/2, 0);
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "multishot terminal (cq 3) then DRAIN (cq 4)");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) == 0, "cq3 (slot 0) = multishot terminal");
    TEST_EXPECT_EQ(cqes[0].user_data, 0x3EA10001u, "terminal is the multishot");
    TEST_EXPECT_EQ(cqes[1].user_data, 0x3EA10002u, "cq4 (slot 1) = the DRAIN, AFTER the terminal");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all done");
    TEST_ASSERT(l->inflight_ops == NULL, "multishot reaped");
    TEST_ASSERT(l->chain == NULL, "DRAIN reclaimed");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-6a: the registered-buffer data-path ops (READ / WRITE) over the loopback
// 9P client. White-box buffer install (Proc-less, like the FSYNC e2e installs
// the handle directly): a fresh anon Burrow's create ref IS the table's pin; the
// test takes ONE EXTRA ref so it can observe the op's I-30 buffer pin being
// taken + released around the dispatch.
// =============================================================================

// Stage a READ/WRITE SQE (all the data-path fields the scalar cl_stage_sqe omits).
static void cl_stage_rw(struct Loom *l, u32 slot, u8 opcode, u32 handle_idx,
                        u64 offset, u32 count, u32 bidx, u64 buf_off, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode        = opcode;
    s->handle_idx    = handle_idx;
    s->offset        = offset;
    s->len           = count;
    s->buf_idx_or_off = bidx;
    s->_resv1[0]     = buf_off;     // LOOM_SQE_BUF_OFF
    s->user_data     = user_data;
    sqa[slot] = slot;
}

// Install a fresh anon Burrow of `len` into l->reg_buf[idx] (its create ref is
// the table's pin) + take one extra ref for the test to observe lifetime.
static void loom_install_test_buf(struct Loom *l, u32 idx, u32 len,
                                  struct Burrow **out_b, u8 **out_kva) {
    struct Burrow *b = burrow_create_anon(len);
    u8 *kva = (u8 *)pa_to_kva(page_to_pa(b->pages));
    spin_lock(&l->lock);
    l->reg_buf[idx].burrow = b;
    l->reg_buf[idx].kva    = kva;
    l->reg_buf[idx].len    = len;
    if (idx + 1u > l->n_reg_buf) l->n_reg_buf = idx + 1u;
    spin_unlock(&l->lock);
    burrow_ref(b);   // the test's observation ref (on top of the table pin)
    *out_b = b; *out_kva = kva;
}

// Capture responder: stash the Twrite payload (to prove the build read the
// pinned buffer), then delegate every reply (incl. the Rwrite count-echo) to
// canonical_responder.
static u8  g_loom_wcap[64];
static u32 g_loom_wcap_len;
static int loom_write_capture_responder(void *ctx, const u8 *req, size_t req_len,
                                        u8 *resp, size_t cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) >= 0 && type == P9_TWRITE &&
        req_len >= P9_HDR_LEN + 16) {
        u32 count = (u32)req[P9_HDR_LEN + 12] | ((u32)req[P9_HDR_LEN + 13] << 8)
                  | ((u32)req[P9_HDR_LEN + 14] << 16) | ((u32)req[P9_HDR_LEN + 15] << 24);
        u32 nn = count > (u32)sizeof(g_loom_wcap) ? (u32)sizeof(g_loom_wcap) : count;
        for (u32 i = 0; i < nn; i++) g_loom_wcap[i] = req[P9_HDR_LEN + 16 + i];
        g_loom_wcap_len = nn;
    }
    return canonical_responder(ctx, req, req_len, resp, cap);
}

// READ end-to-end: the SQE dispatches a Tread; the loopback replies Rread with
// "hello" (5 bytes); loom_async_complete copies the reply payload INTO the
// registered buffer; the CQE result is the byte count. Exercises the new
// completion-time wire->buffer copy + the I-30 buffer pin lifecycle.
void test_9p_client_loom_read_e2e(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);          // test ref + table pin
    bkva[0] = bkva[1] = bkva[2] = bkva[3] = bkva[4] = 0xAA;   // poison

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_READ, /*handle=*/0, /*offset=*/0, /*count=*/5,
                /*bidx=*/0, /*buf_off=*/0, 0xBEEF000000000005ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE posted");
    TEST_EXPECT_EQ(cqes[0].user_data, 0xBEEF000000000005ULL, "user_data echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)5, "READ result = 5 bytes read");
    TEST_ASSERT(bkva[0]=='h' && bkva[1]=='e' && bkva[2]=='l' && bkva[3]=='l' && bkva[4]=='o',
                "Rread payload copied into the registered buffer");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced (released at reap)");

    burrow_unref(b);                // drop the observation ref (table pin remains)
    loom_unref(l);                  // releases the table pin -> the buffer frees
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// WRITE end-to-end: the SQE dispatches a Twrite whose data is read FROM the
// registered buffer in the build thunk; the capture responder proves the buffer
// bytes reached the wire; the CQE result is the server-accepted count.
void test_9p_client_loom_write_e2e(void) {
    g_loom_wcap_len = 0;
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_write_capture_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (capture responder)");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_loopback_ops_for(&g_loopback), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake");

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    const u8 payload[] = {'W','O','R','L','D','!'};
    for (u32 i = 0; i < sizeof(payload); i++) bkva[i] = payload[i];

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_WRITE, /*handle=*/0, /*offset=*/0, /*count=*/(u32)sizeof(payload),
                /*bidx=*/0, /*buf_off=*/0, 0xF00D000000000006ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)sizeof(payload), "WRITE result = accepted count");
    TEST_EXPECT_EQ((u64)g_loom_wcap_len, (u64)sizeof(payload), "server saw the full payload");
    TEST_ASSERT(g_loom_wcap[0]=='W' && g_loom_wcap[1]=='O' && g_loom_wcap[2]=='R' &&
                g_loom_wcap[3]=='L' && g_loom_wcap[4]=='D' && g_loom_wcap[5]=='!',
                "buffer bytes copied build-time onto the wire");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Weft-6c (NET-THROUGHPUT 6; I-37): the zero-copy data drive over Loom. A
// LOOM_OP_READ/WRITE on a /net data fid whose registered buffer IS the per-flow
// shared ring is routed to a Tweftio (off/len/dir descriptor) instead of a byte-
// copying Tread/Twrite -- netd moves the bytes IN PLACE in the shared ring, so the
// kernel copies NOTHING and the Rweftio count is the CQE result. The capture
// responder records which wire op the server saw (Tweftio vs Tread) + the Tweftio
// descriptor, and echoes the requested len back as the moved count.
// =============================================================================
static u8  g_weft_req_type;     // last request type the server saw (0 == none)
static u32 g_weft_off;          // captured Tweftio offset (payload-relative)
static u32 g_weft_len;          // captured Tweftio len
static u32 g_weft_dir;          // captured Tweftio direction (WEFT_DIR_*)
static int loom_weft_capture_responder(void *ctx, const u8 *req, size_t req_len,
                                       u8 *resp, size_t cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) >= 0) {
        g_weft_req_type = type;
        if (type == P9_TWEFTIO && req_len >= P9_HDR_LEN + 16) {
            // Tweftio body: [fid u32][off u32][len u32][dir u32].
            g_weft_off = (u32)req[P9_HDR_LEN+4]  | ((u32)req[P9_HDR_LEN+5]<<8)
                       | ((u32)req[P9_HDR_LEN+6]<<16) | ((u32)req[P9_HDR_LEN+7]<<24);
            g_weft_len = (u32)req[P9_HDR_LEN+8]  | ((u32)req[P9_HDR_LEN+9]<<8)
                       | ((u32)req[P9_HDR_LEN+10]<<16) | ((u32)req[P9_HDR_LEN+11]<<24);
            g_weft_dir = (u32)req[P9_HDR_LEN+12] | ((u32)req[P9_HDR_LEN+13]<<8)
                       | ((u32)req[P9_HDR_LEN+14]<<16) | ((u32)req[P9_HDR_LEN+15]<<24);
            // Rweftio echoing the requested len (proves the len round-trips the wire).
            size_t total = P9_HDR_LEN + 4;
            if (cap < total) return -1;
            resp[0] = (u8)total; resp[1] = 0; resp[2] = 0; resp[3] = 0;
            resp[4] = P9_RWEFTIO;
            resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
            resp[7]  = (u8)(g_weft_len & 0xff);         resp[8]  = (u8)((g_weft_len >> 8) & 0xff);
            resp[9]  = (u8)((g_weft_len >> 16) & 0xff); resp[10] = (u8)((g_weft_len >> 24) & 0xff);
            return (int)total;
        }
    }
    return canonical_responder(ctx, req, req_len, resp, cap);
}

// Open the loopback client with the weft capture responder (mirrors
// drive_client_open, which wires canonical_responder).
static int weft_drive_open(struct p9_client *c, struct p9_loopback *lb) {
    int rc = p9_loopback_init(lb, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_weft_capture_responder, NULL);
    if (rc < 0) return -1;
    rc = p9_client_init(c, 0, 8192, p9_loopback_ops_for(lb),
                        g_recv_buf, sizeof(g_recv_buf));
    if (rc < 0) return -1;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    return p9_client_handshake(c, uname, sizeof(uname), aname, sizeof(aname), 0);
}

#define WEFT_TEST_RING_SIZE   PAGE_SIZE
#define WEFT_TEST_RING_ENTS   8u
#define WEFT_TEST_GUEST_VA    0x40000000ULL

// Install a fresh anon Burrow as BOTH the Loom reg_buf[idx] AND a weft binding on
// `sp`'s dev9p priv -- the SYS_WEFT_MAP'd per-flow ring. The WHOLE Burrow is the
// registered buffer (buf_reg_len == ring_size), so a slice's buf_off is ring-base-
// relative -- the weft routing's whole-ring contract. The binding holds one
// registration pin (burrow_ref), released by dev9p_close -> weft_binding_release
// when `sp` is clunked at loom_unref. Returns the payload-region offset (where a
// zero-copy slice must start); the binding pointer comes back via out_wb so the
// caller asserts the install.
static u32 weft_install_ring(struct Loom *l, u32 idx, struct Spoor *sp,
                             struct Burrow **out_b, u8 **out_kva,
                             struct weft_binding **out_wb) {
    loom_install_test_buf(l, idx, WEFT_TEST_RING_SIZE, out_b, out_kva);
    struct weft_binding *wb = weft_binding_alloc(*out_b, WEFT_TEST_GUEST_VA,
                                                 WEFT_TEST_RING_SIZE, WEFT_TEST_RING_ENTS);
    *out_wb = wb;
    if (!wb) return 0;
    burrow_ref(*out_b);                          // the binding's registration pin
    dev9p_priv_of(sp)->weft = wb;
    return wb->view.payload_off;
}

// Weft READ E2E: a LOOM_OP_READ on a weft-bound fid + the ring buffer routes to a
// Tweftio(dir=READ); the kernel copies NOTHING into the ring slice (netd places the
// recv'd bytes there in place -- the canned reply carries none, so the sentinel must
// survive), and the CQE result is the Rweftio count. A single terminal CQE, no MORE.
void test_9p_client_loom_weft_read_e2e(void) {
    TEST_ASSERT(weft_drive_open(&g_client, &g_loopback) == 0, "weft client open");
    // Walk-bind fid 20 (like a real /net data fid -- a walked, opened fid, never the
    // attach root); an unbound fid would fail fid_bound in p9_session_send_*.
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);
    struct Spoor *sp = dev9p_attach_client(&g_client, 20);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *ringb; u8 *rkva; struct weft_binding *wb;
    u32 poff = weft_install_ring(l, 0, sp, &ringb, &rkva, &wb);
    TEST_ASSERT(wb != NULL, "weft binding installed");
    int hc0 = burrow_handle_count(ringb);
    for (u32 i = 0; i < 256; i++) rkva[poff + i] = 0xAA;   // sentinel (must survive)

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    g_weft_req_type = 0;
    cl_stage_rw(l, 0, LOOM_OP_READ, /*handle=*/0, /*offset=*/0, /*count=*/256,
                /*bidx=*/0, /*buf_off=*/poff, 0xBEEFBEEF00000100ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)g_weft_req_type, (u64)P9_TWEFTIO, "server saw a Tweftio (not Tread)");
    TEST_EXPECT_EQ((u64)g_weft_dir, (u64)WEFT_DIR_READ, "Tweftio dir = READ");
    TEST_EXPECT_EQ((u64)g_weft_off, (u64)0, "Tweftio off = payload-relative 0");
    TEST_EXPECT_EQ((u64)g_weft_len, (u64)256, "Tweftio len = 256");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) == 0, "single terminal CQE (no MORE)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)256, "result = Rweftio count");
    bool intact = true;
    for (u32 i = 0; i < 256; i++) if (rkva[poff + i] != 0xAA) intact = false;
    TEST_ASSERT(intact, "ring slice untouched by the kernel (zero-copy READ)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(ringb), hc0, "op buffer pin balanced");

    burrow_unref(ringb);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Weft WRITE E2E: a LOOM_OP_WRITE routes to a Tweftio(dir=WRITE) carrying ONLY the
// off/len descriptor (no payload on the wire -- netd reads the ring in place); the
// CQE result is the Rweftio count, and the completion is a single terminal CQE (no
// LOOM_CQE_MORE -- the COPIED F_NOTIF realization: at v1.0 netd copies the ring into
// its socket buffer, so the slice is reusable the instant the CQE arrives).
void test_9p_client_loom_weft_write_e2e(void) {
    TEST_ASSERT(weft_drive_open(&g_client, &g_loopback) == 0, "weft client open");
    // Walk-bind fid 20 (like a real /net data fid -- a walked, opened fid, never the
    // attach root); an unbound fid would fail fid_bound in p9_session_send_*.
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);
    struct Spoor *sp = dev9p_attach_client(&g_client, 20);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *ringb; u8 *rkva; struct weft_binding *wb;
    u32 poff = weft_install_ring(l, 0, sp, &ringb, &rkva, &wb);
    TEST_ASSERT(wb != NULL, "weft binding installed");
    int hc0 = burrow_handle_count(ringb);
    for (u32 i = 0; i < 512; i++) rkva[poff + i] = (u8)i;   // the guest's payload, in the ring

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    g_weft_req_type = 0;
    cl_stage_rw(l, 0, LOOM_OP_WRITE, /*handle=*/0, /*offset=*/0, /*count=*/512,
                /*bidx=*/0, /*buf_off=*/poff, 0xF00DF00D00000200ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)g_weft_req_type, (u64)P9_TWEFTIO, "server saw a Tweftio (not Twrite)");
    TEST_EXPECT_EQ((u64)g_weft_dir, (u64)WEFT_DIR_WRITE, "Tweftio dir = WRITE");
    TEST_EXPECT_EQ((u64)g_weft_len, (u64)512, "Tweftio len = 512");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE");
    TEST_ASSERT((cqes[0].flags & LOOM_CQE_MORE) == 0,
                "single terminal CQE, no MORE (copied F_NOTIF realization)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)512, "result = Rweftio count");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(ringb), hc0, "op buffer pin balanced");

    burrow_unref(ringb);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Weft hybrid fallback (section 4.8): a LOOM_OP_READ on a weft-bound fid but with a
// NON-ring registered buffer (slot 1, a separate Burrow) falls through to the byte
// path -- a normal Tread, NOT a Tweftio. Only the per-flow ring goes zero-copy; a
// small/control transfer over a scratch buffer stays byte-copy, and its reply is
// copied into the buffer as usual.
void test_9p_client_loom_weft_hybrid_fallback(void) {
    TEST_ASSERT(weft_drive_open(&g_client, &g_loopback) == 0, "weft client open");
    // Walk-bind fid 20 (like a real /net data fid -- a walked, opened fid, never the
    // attach root); an unbound fid would fail fid_bound in p9_session_send_*.
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);
    struct Spoor *sp = dev9p_attach_client(&g_client, 20);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *ringb; u8 *rkva; struct weft_binding *wb;
    (void)weft_install_ring(l, 0, sp, &ringb, &rkva, &wb);   // ring + binding -> slot 0
    TEST_ASSERT(wb != NULL, "weft binding installed");
    struct Burrow *plain; u8 *pkva;
    loom_install_test_buf(l, 1, PAGE_SIZE, &plain, &pkva);   // a NON-ring buffer -> slot 1
    pkva[0] = 0x00;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    g_weft_req_type = 0;
    // bidx = 1 (the NON-ring buffer) -> the weft routing does not fire.
    cl_stage_rw(l, 0, LOOM_OP_READ, /*handle=*/0, /*offset=*/0, /*count=*/5,
                /*bidx=*/1, /*buf_off=*/0, 0xCAFE000000000005ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)g_weft_req_type, (u64)P9_TREAD, "non-ring buffer -> byte Tread, not Tweftio");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)5, "byte READ result = 5 (hello)");
    TEST_ASSERT(pkva[0] == 'h' && pkva[4] == 'o', "byte path copied the reply into the buffer");

    burrow_unref(ringb);
    burrow_unref(plain);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Weft OOB rejection: a weft op whose slice lands in the ring's CONTROL region
// (buf_off < payload_off) is rejected at submit (-EINVAL) -- the same bounds gate
// the synchronous dev9p_weft_try_rw uses. No Tweftio is sent (rejected before the
// engine), proving the validator-once runs on the kernel SQE snapshot.
void test_9p_client_loom_weft_oob_rejected(void) {
    TEST_ASSERT(weft_drive_open(&g_client, &g_loopback) == 0, "weft client open");
    // Walk-bind fid 20 (like a real /net data fid -- a walked, opened fid, never the
    // attach root); an unbound fid would fail fid_bound in p9_session_send_*.
    p9_client_walk_one(&g_client, 0, 20, (const u8 *)"d", 1, NULL);
    struct Spoor *sp = dev9p_attach_client(&g_client, 20);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *ringb; u8 *rkva; struct weft_binding *wb;
    u32 poff = weft_install_ring(l, 0, sp, &ringb, &rkva, &wb);
    TEST_ASSERT(wb != NULL && poff > 0, "binding installed; payload after the control region");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    g_weft_req_type = 0;
    // buf_off = 0 lands in the control region [0, payload_off) -> rejected.
    cl_stage_rw(l, 0, LOOM_OP_WRITE, /*handle=*/0, /*offset=*/0, /*count=*/64,
                /*bidx=*/0, /*buf_off=*/0, 0xDEAD000000000040ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)g_weft_req_type, (u64)0, "no wire op sent (rejected at submit)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE (the inline rejection)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL),
                   "in-ring slice outside payload -> -EINVAL");

    burrow_unref(ringb);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Submit-time rejections (inline CQEs; the op NEVER goes in flight): a bad
// registered-buffer index, an out-of-bounds slice, and a READ against a handle
// whose rights snapshot lacks RIGHT_READ. The I-30 gates run at submit and a
// rejected op is never dispatched, so it cannot be bypassed by a later mutation.
void test_9p_client_loom_rw_rejects(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // NO RIGHT_READ -> a READ is denied
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write-only handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // (1) bad buffer index (n_reg_buf == 1, ask for slot 5) -> -EINVAL.
    cl_stage_rw(l, 0, LOOM_OP_WRITE, 0, 0, 4, /*bidx=*/5, 0, 0x1111u);
    // (2) OOB slice (buf_off at the buffer end, count 1) -> -EINVAL.
    cl_stage_rw(l, 1, LOOM_OP_WRITE, 0, 0, 1, /*bidx=*/0, /*buf_off=*/PAGE_SIZE, 0x2222u);
    // (3) READ against the write-only handle -> -EACCES.
    cl_stage_rw(l, 2, LOOM_OP_READ, 0, 0, 4, /*bidx=*/0, 0, 0x3333u);
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 3, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 3, "three SQEs consumed (all rejected inline)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "three inline CQEs");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL), "bad buf idx -> -EINVAL");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_INVAL), "OOB slice -> -EINVAL");
    TEST_EXPECT_EQ((u64)(s64)cqes[2].result, (u64)(s64)(-(s32)T_E_ACCES), "READ on write-only -> -EACCES");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no op ever went in flight");
    TEST_ASSERT(l->inflight_ops == NULL, "no async container allocated");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-6b-1: the read-shaped payload ops over the registered-buffer machinery.
// READDIR / READLINK stream bytes into the dest buffer (the READ pattern);
// GETATTR / STATFS copy a fixed parsed record. All single-fid, RIGHT_READ.
// =============================================================================

// Responder that returns a known 8-byte Rreaddir dirent stream (the canonical
// responder returns count=0, which wouldn't exercise the completion copy).
// Every other reply delegates to canonical_responder.
static int loom_readdir_responder(void *ctx, const u8 *req, size_t req_len,
                                  u8 *resp, size_t cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) >= 0 && type == P9_TREADDIR) {
        const u8 blob[] = {0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8};
        size_t total = P9_HDR_LEN + 4 + sizeof(blob);
        if (cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREADDIR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)sizeof(blob); resp[8] = 0; resp[9] = 0; resp[10] = 0;  // count = 8
        for (size_t i = 0; i < sizeof(blob); i++) resp[11 + i] = blob[i];
        return (int)total;
    }
    return canonical_responder(ctx, req, req_len, resp, cap);
}

// READDIR end-to-end: an Rreaddir dirent stream is copied INTO the registered
// buffer (the READ pattern with op_offset = dir read offset); result = byte
// count. Uses the custom responder so the stream is non-empty.
void test_9p_client_loom_readdir_e2e(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_readdir_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (readdir responder)");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_loopback_ops_for(&g_loopback), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    const u8 uname[] = {'r','o','o','t'}; const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake");

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    for (int i = 0; i < 8; i++) bkva[i] = 0;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_READDIR, /*handle=*/0, /*offset=*/0, /*count=*/64,
                /*bidx=*/0, /*buf_off=*/0, 0xD11D000000000008ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)8, "READDIR result = 8 dirent bytes");
    TEST_ASSERT(bkva[0]==0xD1 && bkva[3]==0xD4 && bkva[7]==0xD8,
                "Rreaddir stream copied into the registered buffer");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// READLINK end-to-end: the link target ("/tmp") is copied INTO the dest buffer;
// result = its length. op_count is the dest capacity (no request count).
void test_9p_client_loom_readlink_e2e(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    for (int i = 0; i < 4; i++) bkva[i] = 0;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_READLINK, /*handle=*/0, /*offset=*/0, /*cap=*/64,
                /*bidx=*/0, /*buf_off=*/0, 0x1117000000000004ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)4, "READLINK result = 4 bytes (/tmp)");
    TEST_ASSERT(bkva[0]=='/' && bkva[1]=='t' && bkva[2]=='m' && bkva[3]=='p',
                "link target copied into the registered buffer");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// GETATTR end-to-end: the parsed struct p9_attr is copied into the dest buffer
// (op_offset = request_mask); result = sizeof(struct p9_attr). The canonical
// responder fills mode=0644 / size=128 / valid=BASIC -- assert they round-trip.
void test_9p_client_loom_getattr_e2e(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_GETATTR, /*handle=*/0, /*request_mask=*/P9_GETATTR_BASIC,
                /*cap=*/(u32)sizeof(struct p9_attr), /*bidx=*/0, /*buf_off=*/0,
                0x6E11000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)sizeof(struct p9_attr),
                   "GETATTR result = sizeof(struct p9_attr)");
    struct p9_attr *a = (struct p9_attr *)bkva;
    TEST_EXPECT_EQ((u64)a->valid, (u64)P9_GETATTR_BASIC, "getattr valid mask copied");
    TEST_EXPECT_EQ((u64)a->mode, (u64)0x1A4u, "getattr mode copied (0644)");
    TEST_EXPECT_EQ((u64)a->size, (u64)128u, "getattr size copied");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// STATFS end-to-end: the parsed struct p9_statfs is copied into the dest buffer;
// result = sizeof(struct p9_statfs). The canonical responder fills bsize=4096.
void test_9p_client_loom_statfs_e2e(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_rw(l, 0, LOOM_OP_STATFS, /*handle=*/0, /*offset=*/0,
                /*cap=*/(u32)sizeof(struct p9_statfs), /*bidx=*/0, /*buf_off=*/0,
                0x57F5000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)sizeof(struct p9_statfs),
                   "STATFS result = sizeof(struct p9_statfs)");
    struct p9_statfs *st = (struct p9_statfs *)bkva;
    TEST_EXPECT_EQ((u64)st->bsize, (u64)4096u, "statfs bsize copied");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Submit-time rejection on the read-shaped ops: GETATTR (and every read-shaped
// op) requires RIGHT_READ on the registered handle, and a bad registered-buffer
// index is rejected -- proving the 6a I-30 gates apply uniformly to the 6b ops.
void test_9p_client_loom_metaread_rejects(void) {
    drive_client_open(&g_client, &g_loopback);
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // NO RIGHT_READ -> a read-shaped op is denied
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write-only handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // (1) GETATTR on the write-only handle -> -EACCES.
    cl_stage_rw(l, 0, LOOM_OP_GETATTR, 0, P9_GETATTR_BASIC,
                (u32)sizeof(struct p9_attr), /*bidx=*/0, 0, 0x1u);
    // (2) READDIR with a bad registered-buffer index -> -EINVAL.
    cl_stage_rw(l, 1, LOOM_OP_READDIR, 0, 0, 64, /*bidx=*/9, 0, 0x2u);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 2, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 2, "two SQEs consumed (both rejected inline)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_ACCES),
                   "GETATTR without RIGHT_READ -> -EACCES");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_INVAL),
                   "READDIR bad buf idx -> -EINVAL");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no op ever went in flight");
    TEST_ASSERT(l->inflight_ops == NULL, "no async container allocated");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-6b-2: the metadata-MUTATION ops (SETATTR / MKDIR / MKNOD / SYMLINK /
// UNLINKAT / RENAMEAT / LINK). Each reads its name(s) / input struct FROM the
// pinned registered buffer (the WRITE-payload discipline); the two-fid ops
// (RENAMEAT / LINK) pin a SECOND registered handle. The reply is scalar
// (0 / -errno). White-box install of the registered handle(s) + buffer, like the
// 6a/6b-1 e2e tests.
// =============================================================================

// Install `sp` into l->reg[idx] with `rights`, taking ONE extra ref so the test
// can register the same Spoor in two slots (the two-fid ops) without
// double-adopting dev9p_attach_client's single ref. loom_unref clunks both.
static void loom_install_test_handle(struct Loom *l, u32 idx, struct Spoor *sp,
                                     rights_t rights) {
    spoor_ref(sp);
    spin_lock(&l->lock);
    l->reg[idx].spoor  = sp;
    l->reg[idx].rights = rights;
    spin_unlock(&l->lock);
}

// Stage a mutation SQE: the full field set incl. the reserved-tail scalars +
// name sub-lengths (_resv1[1]/_resv1[2]) + the second-handle index (_resv1[3]).
static void cl_stage_mut(struct Loom *l, u32 slot, u8 opcode, u32 handle_idx,
                         u64 offset, u32 len, u32 bidx, u64 buf_off,
                         u64 r1, u64 r2, u64 r3, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode         = opcode;
    s->handle_idx     = handle_idx;
    s->offset         = offset;
    s->len            = len;
    s->buf_idx_or_off = bidx;
    s->_resv1[0]      = buf_off;     // LOOM_SQE_BUF_OFF
    s->_resv1[1]      = r1;
    s->_resv1[2]      = r2;
    s->_resv1[3]      = r3;          // LOOM_SQE_FID2 (two-fid ops)
    s->user_data      = user_data;
    sqa[slot] = slot;
}

// Capture responder for the mutation tests: stash the on-wire name(s) / struct
// fields the build thunk read out of the pinned buffer (proving the buffer bytes
// reached the wire), then delegate every reply to canonical_responder.
static u8  g_loom_mname[64];  static u32 g_loom_mname_len;
static u32 g_loom_mname_mode;
static u8  g_loom_mname2[64]; static u32 g_loom_mname2_len;
static u32 g_loom_msetattr_valid; static u32 g_loom_msetattr_mode;
static u32 loom_rd_le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 loom_rd_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void loom_cap_name(u8 *dst, u32 *dlen, const u8 *src, u32 n) {
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
    *dlen = n;
}
static int loom_mut_capture_responder(void *ctx, const u8 *req, size_t req_len,
                                      u8 *resp, size_t cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) >= 0) {
        if (type == P9_TMKDIR && req_len >= (size_t)P9_HDR_LEN + 6) {
            u32 nl = loom_rd_le16(req + P9_HDR_LEN + 4);              // name s len
            if (req_len >= (size_t)P9_HDR_LEN + 6 + nl + 4) {
                loom_cap_name(g_loom_mname, &g_loom_mname_len, req + P9_HDR_LEN + 6, nl);
                g_loom_mname_mode = loom_rd_le32(req + P9_HDR_LEN + 6 + nl);  // mode after name
            }
        } else if (type == P9_TSETATTR && req_len >= (size_t)P9_HDR_LEN + 12) {
            g_loom_msetattr_valid = loom_rd_le32(req + P9_HDR_LEN + 4);   // valid u32
            g_loom_msetattr_mode  = loom_rd_le32(req + P9_HDR_LEN + 8);   // mode u32
        } else if (type == P9_TRENAMEAT && req_len >= (size_t)P9_HDR_LEN + 6) {
            u32 onl = loom_rd_le16(req + P9_HDR_LEN + 4);            // oldname s len
            size_t after_old = (size_t)P9_HDR_LEN + 6 + onl;        // -> newdirfid
            if (req_len >= after_old)
                loom_cap_name(g_loom_mname, &g_loom_mname_len, req + P9_HDR_LEN + 6, onl);
            if (req_len >= after_old + 6) {
                u32 nnl = loom_rd_le16(req + after_old + 4);         // newname s len
                if (req_len >= after_old + 6 + nnl)
                    loom_cap_name(g_loom_mname2, &g_loom_mname2_len, req + after_old + 6, nnl);
            }
        }
    }
    return canonical_responder(ctx, req, req_len, resp, cap);
}

// MKDIR end-to-end: the name + mode are read FROM the pinned buffer / the SQE;
// the capture responder proves both reached the wire; the scalar Rmkdir (qid
// dropped at v1.0) -> result 0. Exercises the name-from-buffer mechanism + the
// per-op scalar decode + the RIGHT_WRITE dir gate.
void test_9p_client_loom_mkdir_e2e(void) {
    g_loom_mname_len = 0; g_loom_mname_mode = 0;
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_mut_capture_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (mut capture)");
    // Drive the open MANUALLY (not drive_client_open, which would reset the
    // loopback to the canonical responder + clobber the capture): the handshake
    // rides the capture responder, which delegates Tversion/Tattach to canonical.
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_loopback_ops_for(&g_loopback), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake");
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // create requires RIGHT_WRITE on the dir
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write dir handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    const char *nm = "subdir";
    for (u32 i = 0; i < 6; i++) bkva[i] = (u8)nm[i];

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // MKDIR: handle=0 (dir); region [0,6) = the name; _resv1[1]=mode 0755, _resv1[2]=gid.
    cl_stage_mut(l, 0, LOOM_OP_MKDIR, /*handle=*/0, /*offset=*/0, /*len=*/6,
                 /*bidx=*/0, /*buf_off=*/0, /*mode=*/0755u, /*gid=*/0, /*fid2=*/0,
                 0xD1D0000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "MKDIR result = 0 (scalar success)");
    TEST_EXPECT_EQ((u64)g_loom_mname_len, (u64)6, "mkdir name length on wire");
    TEST_ASSERT(g_loom_mname[0]=='s' && g_loom_mname[3]=='d' && g_loom_mname[5]=='r',
                "mkdir name bytes read from the pinned buffer");
    TEST_EXPECT_EQ((u64)g_loom_mname_mode, (u64)0755u, "mkdir mode decoded from the SQE");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// SETATTR end-to-end: the input struct p9_setattr is read FROM the pinned buffer
// (align-safe copy); the capture responder proves valid + mode reached the wire;
// Rsetattr (empty) -> result 0. Exercises the struct-from-buffer input path.
void test_9p_client_loom_setattr_e2e(void) {
    g_loom_msetattr_valid = 0; g_loom_msetattr_mode = 0;
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_mut_capture_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (mut capture)");
    // Drive the open MANUALLY (not drive_client_open, which would reset the
    // loopback to the canonical responder + clobber the capture): the handshake
    // rides the capture responder, which delegates Tversion/Tattach to canonical.
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_loopback_ops_for(&g_loopback), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake");
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // setattr mutates metadata -> RIGHT_WRITE
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    // Write a struct p9_setattr into the registered buffer (valid=MODE, mode=0600).
    struct p9_setattr *sa = (struct p9_setattr *)bkva;
    for (u32 i = 0; i < sizeof(*sa); i++) ((u8 *)sa)[i] = 0;
    sa->valid = P9_SETATTR_MODE; sa->mode = 0600u;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_mut(l, 0, LOOM_OP_SETATTR, /*handle=*/0, /*offset=*/0,
                 /*len=*/(u32)sizeof(struct p9_setattr), /*bidx=*/0, /*buf_off=*/0,
                 0, 0, 0, 0x5E77000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "SETATTR result = 0 (scalar success)");
    TEST_EXPECT_EQ((u64)g_loom_msetattr_valid, (u64)P9_SETATTR_MODE, "setattr valid mask on wire");
    TEST_EXPECT_EQ((u64)g_loom_msetattr_mode, (u64)0600u, "setattr mode read from the buffer");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// RENAMEAT end-to-end: a TWO-FID op. olddir = reg slot 0, newdir = reg slot 1
// (the same Spoor registered twice). The buffer holds oldname ++ newname; the
// split rides _resv1[1]; the second handle index rides _resv1[3]. The capture
// responder proves BOTH names reached the wire; Rrenameat (empty) -> result 0.
// Exercises the second-fid resolve+pin (two I-30 pins) + the two-name split.
void test_9p_client_loom_renameat_e2e(void) {
    g_loom_mname_len = 0; g_loom_mname2_len = 0;
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                              loom_mut_capture_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (mut capture)");
    // Drive the open MANUALLY (not drive_client_open, which would reset the
    // loopback to the canonical responder + clobber the capture): the handshake
    // rides the capture responder, which delegates Tversion/Tattach to canonical.
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_loopback_ops_for(&g_loopback), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake");
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register olddir (slot 0)");
    loom_install_test_handle(l, 1, sp, RIGHT_WRITE);   // newdir (slot 1; same Spoor)

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    bkva[0]='o'; bkva[1]='l'; bkva[2]='d'; bkva[3]='n'; bkva[4]='e'; bkva[5]='w';

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // RENAMEAT: handle=0 (olddir); region [0,6) = oldname ++ newname; _resv1[1]=3
    // (oldname_len split); _resv1[3]=1 (newdir handle index).
    cl_stage_mut(l, 0, LOOM_OP_RENAMEAT, /*handle=*/0, /*offset=*/0, /*len=*/6,
                 /*bidx=*/0, /*buf_off=*/0, /*oldname_len=*/3, /*r2=*/0, /*fid2=*/1,
                 0xBE77000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "RENAMEAT result = 0 (scalar success)");
    TEST_EXPECT_EQ((u64)g_loom_mname_len, (u64)3, "oldname length on wire");
    TEST_ASSERT(g_loom_mname[0]=='o' && g_loom_mname[1]=='l' && g_loom_mname[2]=='d',
                "oldname bytes from the pinned buffer [0,3)");
    TEST_EXPECT_EQ((u64)g_loom_mname2_len, (u64)3, "newname length on wire");
    TEST_ASSERT(g_loom_mname2[0]=='n' && g_loom_mname2[1]=='e' && g_loom_mname2[2]=='w',
                "newname bytes from the pinned buffer [3,6)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "both fid pins released at reap");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Submit-time rejections for the mutation ops -- the rights gate + the
// memory-safety guards, all inline (no op goes async):
//   (1) MKDIR on a RIGHT_READ-only dir -> -EACCES (mirrors the create gate);
//   (2) RENAMEAT with a second-handle index out of range -> -EINVAL;
//   (3) SYMLINK with name_len (_resv1[1]) > the pinned span -> -EINVAL (the
//       two-name split overrun guard);
//   (4) SETATTR with a span shorter than struct p9_setattr -> -EINVAL.
void test_9p_client_loom_mutation_rejects(void) {
    drive_client_open(&g_client, &g_loopback);   // canonical responder
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_READ;            // READ-only: a mutation op is denied
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register read-only handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    cl_stage_mut(l, 0, LOOM_OP_MKDIR, 0, 0, /*len=*/4, /*bidx=*/0, 0, 0755u, 0, 0, 0x1u);
    cl_stage_mut(l, 1, LOOM_OP_RENAMEAT, 0, 0, /*len=*/4, /*bidx=*/0, 0,
                 /*oldname_len=*/2, 0, /*fid2=*/LOOM_MAX_REG_HANDLES, 0x2u);
    cl_stage_mut(l, 2, LOOM_OP_SYMLINK, 0, 0, /*len=*/4, /*bidx=*/0, 0,
                 /*name_len=*/9, 0, 0, 0x3u);   // name_len > span
    cl_stage_mut(l, 3, LOOM_OP_SETATTR, 0, 0, /*len=*/8, /*bidx=*/0, 0, 0, 0, 0, 0x4u);
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 4, "four SQEs consumed (all rejected inline)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_ACCES),
                   "MKDIR without RIGHT_WRITE -> -EACCES");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_INVAL),
                   "RENAMEAT bad second-handle index -> -EINVAL");
    TEST_EXPECT_EQ((u64)(s64)cqes[2].result, (u64)(s64)(-(s32)T_E_INVAL),
                   "SYMLINK name_len > span -> -EINVAL (overrun guard)");
    TEST_EXPECT_EQ((u64)(s64)cqes[3].result, (u64)(s64)(-(s32)T_E_INVAL),
                   "SETATTR span < struct -> -EINVAL");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no op ever went in flight");
    TEST_ASSERT(l->inflight_ops == NULL, "no async container allocated");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Loom-6c: multi-in-flight. The single-slot p9_loopback refuses a second send
// while a reply is undrained, so it can hold only ONE 9P RPC in flight -- the
// reason the prior Loom tests are single-op / synthetic-NOP and the multi-in-
// flight window stayed reasoned-by-inspection, never test-reproduced (the gap the
// Loom audits carried since #841). The queueing p9_mq_loopback (a byte FIFO that
// stages N replies) closes it: these tests submit N async ops that ALL go in
// flight at once, then complete them all -- driving the multi-entry inflight_ops
// list, async_inflight > 1, the loom_first_inflight_client borrow-guard across a
// real pump, and the multi-entry loom_reap_terminal. The borrow-guard balance is
// asserted deterministically by "the registered Spoor frees exactly once" -- a
// missing guard clunk would leak it (delta 0), a double would have freed it early.
// (The CONCURRENT two-thread reap-vs-pump race + cross-Proc death is the Loom-6d
// native-driver + restored-TSan harness; here the single elected reader drains a
// deterministically-staged multi-in-flight queue.)
// =============================================================================
static struct p9_mq_loopback g_mq;

// N FSYNC ops, all in flight, then all completed. Each Tfsync carries a distinct
// tag; the queueing transport stages N Rfsync; the elected reader demuxes each to
// its op by tag and posts N CQEs. Asserts every op completed exactly once (a
// bitmask over the echoed user_data) + the multi-op reap + the pin balance.
void test_9p_client_loom_multi_inflight_e2e(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake over mq transport");

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    const u32 N = 6;            // <= cq_entries (16); all admit, all in flight at once
    const u64 base = 0xA1F00000ULL;
    for (u32 i = 0; i < N; i++)
        cl_stage_sqe(l, i, LOOM_OP_FSYNC, /*handle=*/0, /*len=datasync*/0, base + i);
    __atomic_store_n(&h->sq_tail, N, __ATOMIC_RELEASE);

    int n = loom_enter(l, /*to_submit=*/N, /*min_complete=*/N, /*flags=*/0);
    TEST_EXPECT_EQ(n, (int)N, "all N SQEs consumed in one enter");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)N, "N CQEs posted (all in-flight ops completed)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all ops reaped");
    TEST_ASSERT(l->inflight_ops == NULL, "all async containers reclaimed");
    // Each op completed EXACTLY once: collect the echoed user_data into a bitmask.
    // A lost reply (demux miss) leaves a bit clear; a double-complete is caught by
    // cq_tail == N above (no extra CQE).
    u32 seen = 0;
    for (u32 i = 0; i < N; i++) {
        u64 ud = cqes[i].user_data;
        TEST_ASSERT(ud >= base && ud < base + N, "CQE user_data in the submitted range");
        TEST_EXPECT_EQ((u64)(s64)cqes[i].result, (u64)0, "fsync success -> result 0");
        seen |= (1u << (u32)(ud - base));
    }
    TEST_EXPECT_EQ((u64)seen, (u64)((1u << N) - 1u),
                   "every one of the N distinct ops completed exactly once (tag-demux)");

    // Pin balance: the registered ref + each of the N in-flight op pins + every
    // borrow-guard ref taken during the pump must all be released. The dev9p root
    // Spoor frees EXACTLY once at loom_unref -- a leaked guard ref would prevent it.
    u64 freed0 = spoor_total_freed();
    loom_unref(l);
    TEST_EXPECT_EQ(spoor_total_freed() - freed0, (u64)1,
                   "dev9p root spoor freed once (all pins + borrow-guards balanced)");
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// N READ ops in flight at once, each into a distinct slice of ONE registered
// buffer. The queueing transport replies Rread("hello") to each; the elected
// reader copies each payload into its op's pinned slice at completion. Drives the
// completion-time wire->buffer copy (loom_payload_result) under multi-in-flight:
// N independent buffer pins, N copies, each clamped to its slice -- and asserts no
// crosstalk (each slice gets its own "hello", the inter-slice gap stays poison).
void test_9p_client_loom_multi_inflight_read_e2e(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake over mq transport");

    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register dev9p spoor");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    for (u32 i = 0; i < 64; i++) bkva[i] = 0xAA;     // poison the whole region

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    const u32 N = 4;
    const u32 STRIDE = 8;       // distinct 8-byte slices (5-byte "hello" + a poison gap)
    const u64 base = 0xBEAD0000ULL;
    for (u32 i = 0; i < N; i++)
        cl_stage_rw(l, i, LOOM_OP_READ, /*handle=*/0, /*offset=*/0, /*count=*/5,
                    /*bidx=*/0, /*buf_off=*/(u64)i * STRIDE, base + i);
    __atomic_store_n(&h->sq_tail, N, __ATOMIC_RELEASE);

    int n = loom_enter(l, N, N, 0);
    TEST_EXPECT_EQ(n, (int)N, "all N READ SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)N, "N CQEs posted");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "all ops reaped");
    for (u32 i = 0; i < N; i++) {
        u64 ud = cqes[i].user_data;
        TEST_ASSERT(ud >= base && ud < base + N, "CQE user_data in range");
        TEST_EXPECT_EQ((u64)(s64)cqes[i].result, (u64)5, "each READ copied 5 bytes");
    }
    // Each slice got its own "hello"; the inter-slice gap stayed poison (no copy
    // crosstalk between the N concurrent in-flight buffer pins).
    for (u32 i = 0; i < N; i++) {
        u8 *s = bkva + (u32)i * STRIDE;
        TEST_ASSERT(s[0]=='h' && s[1]=='e' && s[2]=='l' && s[3]=='l' && s[4]=='o',
                    "slice received its own Rread payload");
        TEST_EXPECT_EQ((u64)s[5], (u64)0xAA, "inter-slice gap unmodified (no copy overrun)");
    }
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "all N buffer pins balanced (released at reap)");

    burrow_unref(b);
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// FID-LIFECYCLE async-clunk F1 regression (the arc audit): a >64-fd async-close
// BURST with NO interleaved sync op must NOT leak bound fids. Pre-fix, the tag
// pool (64 slots) filled with undrained ownerless Rclunks; alloc_tag then failed
// and p9_session_send_clunk returned BEFORE fid_unbind, so closes 65..N left
// their fids bound forever (-> bound_fids[] exhaustion -> a shared-mount DoS).
// The mq transport stages every unread Rclunk (a single-slot loopback cannot),
// so the pool genuinely fills; the fix's client_drain_until_free_tag pumps one
// ownerless reply to free a tag before each over-full send. NON-VACUOUS: with
// the drain reverted, n_bound_fids stays at 1 + (N - 64) instead of 1.
void test_9p_client_async_clunk_burst_no_fid_leak(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname),
                                       aname, sizeof(aname), 0),
                   0, "handshake");
    // Baseline: only the root fid is bound.
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session), (u64)1,
                   "baseline: root bound");

    // Bind N > 64 distinct fids (each Twalk is a sync op that drains its own
    // Rwalk, so the pool is EMPTY before the burst).
    const u32 N = 70;   // > P9_SESSION_MAX_OUTSTANDING (64)
    for (u32 i = 0; i < N; i++) {
        struct p9_qid q;
        const u8 nm[] = {'f'};
        TEST_EXPECT_EQ(p9_client_walk_one(&g_client, /*src=*/0, /*new=*/(u32)(i + 1),
                                          nm, sizeof(nm), &q), 0, "walk binds a fid");
    }
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session), (u64)(N + 1),
                   "N + root fids bound");

    // THE BURST: async-clunk all N with NO interleaved sync op. Closes 65..N hit
    // a full pool; the F1 drain must free a tag for each.
    for (u32 i = 0; i < N; i++)
        TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, (u32)(i + 1)), 0,
                       "async clunk succeeds (drains the full pool)");

    // Every burst-closed fid is UNBOUND -> back to the root-only baseline. A leak
    // would leave (N - 64) fids bound.
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session), (u64)1,
                   "all burst-closed fids unbound (no F1 leak)");

    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// =============================================================================
// #349: a transiently-FULL c2s ring is flow-control, NOT session death. Under
// #841 pipelining + concurrent large frames the kernel->server c2s ring can fill
// momentarily; pre-fix, srvconn collapsed ring-full to -1 and client_run marked
// the WHOLE session dead -- killing every in-flight op, including a peer's text
// page-in (the go-build snare:bus + EIO cascade). The fix (client_send_flow)
// treats P9_TRANSPORT_EAGAIN as back-pressure: it drains the reply path (self-
// pumps when no reader, else parks on a per-sender waiter) so the server frees a
// c2s slot, then RETRIES the send -- the session stays live.
//
// This drives the FAITHFUL production shape on the mq transport (eagain_budget):
// a PRIOR op A (async Tclunk) is in flight with its Rclunk queued; a sync op B
// (Twalk) hits one armed EAGAIN; B self-pumps -> drains A's reply (completing A)
// -> retries -> B succeeds. Both complete, the session is never marked dead.
// Pre-fix this fails by construction: B's EAGAIN -> -1 -> client_mark_dead, so B
// returns -P9_E_IO and A is completed with -P9_E_IO (a dead session), not 0.
// =============================================================================
void test_9p_client_send_backpressure_self_pump(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake over mq transport");

    // The async-op completion records into a Loom CQ (reuse the proven harness).
    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    // Bind fid 20 so op A (an async Tclunk(20)) is well-formed.
    TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, 20, (const u8 *)"f", 1, NULL), 0,
                   "walk binds fid 20 (sync; drains clean)");

    // Op A: async Tclunk(20). Its Rclunk is queued in the FIFO and A stays in
    // flight (no reader has pumped) -- the prior op whose reply B's self-pump drains.
    g_async_op.loom           = l;
    g_async_op.user_data      = 0xA0A0A0A0ULL;
    g_async_op.last_result    = 0x7fffffff;
    g_async_op.completed      = false;
    g_async_op.rpc.on_complete = test_async_on_complete;
    u32 fid_a = 20;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid_a);
    TEST_EXPECT_EQ(rc, 0, "submit_async(clunk A): A in flight, its reply queued");
    TEST_ASSERT(!g_async_op.completed, "A not completed before B's self-pump drains it");

    // Arm ONE EAGAIN: op B's NEXT send hits a transiently-full-but-alive ring.
    g_mq.eagain_budget = 1;

    // Op B: sync Twalk(21). Its send EAGAINs once -> client_send_flow self-pumps
    // (drains A's Rclunk, completing A) -> retries -> B is accepted and completes.
    rc = p9_client_walk_one(&g_client, 0, 21, (const u8 *)"g", 1, NULL);
    TEST_EXPECT_EQ(rc, 0, "sync op B survives c2s back-pressure (self-pump + retry)");

    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the armed EAGAIN actually fired");
    TEST_ASSERT(g_async_op.completed, "op A completed (its reply drained by B's self-pump)");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)0,
                   "op A completed with SUCCESS, not -EIO (the session never died)");
    TEST_ASSERT(!g_client.dead, "session stayed LIVE through the back-pressure");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// =============================================================================
// #349 R2-F1: the send-flow park is MULTI-WAITER. N Procs share one dev9p client
// (docs/reference/47-9p-client.md), so N senders can be back-pressured at once
// and park concurrently. The original fix parked them on a single `Rendez`, which
// extincts on the 2nd sleeper (rendez.h "Extincts on second sleeper") -- an
// unprivileged SMP panic on exactly the #349 workload (parallel writers filling
// the shared c2s while a third op reads). The fix parks each on its OWN stack
// Rendez via a poll_waiter on c->send_waiters_list; the reader's
// client_send_progress_signal wakes them ALL.
//
// This deterministically exercises the multi-waiter wake the park branch relies
// on: register 2 send-waiters, then run the client_send_progress_signal body
// (bump the generation + wake the list) and assert BOTH are woken with NO
// extinction -- the structural guard against the F1 single-waiter regression (a
// single Rendez could not even hold the 2nd register). The full concurrent
// park->retry->complete loop needs 2 live threads racing the shared client (the
// SMP-gate workload + the deterministic multi-thread harness owed since
// #841/#845/Loom); here the multi-waiter mechanism itself is proven directly.
// =============================================================================
void test_9p_client_send_backpressure_multi_waiter(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init (send_waiters_list initialized)");

    // Two concurrent back-pressured senders, each parked on its OWN stack Rendez
    // via a hook on the shared send-waiter list. A single Rendez would extinct at
    // the 2nd register/sleeper; the poll_waiter_list holds N.
    struct Rendez      r1, r2;
    struct poll_waiter pw1, pw2;
    rendez_init(&r1);
    rendez_init(&r2);
    poll_waiter_init(&pw1, &r1);
    poll_waiter_init(&pw2, &r2);
    u64 gen = g_client.send_progress;
    poll_waiter_list_register(&g_client.send_waiters_list, &pw1);
    poll_waiter_list_register(&g_client.send_waiters_list, &pw2);
    g_client.send_waiters = 2;

    // A reader makes progress -- the client_send_progress_signal body: bump the
    // generation, then wake EVERY parked sender. No extinction => the multi-waiter
    // holds 2; both `ready` => both woken; the bumped generation => each parked
    // sender's send_wait_cond (send_progress != gen) would now flip true on retry.
    g_client.send_progress++;
    poll_waiter_list_wake(&g_client.send_waiters_list);

    TEST_ASSERT(pw1.ready, "sender 1 woken by the shared-list wake");
    TEST_ASSERT(pw2.ready, "sender 2 woken (no single-waiter extinction at the 2nd parker)");
    TEST_ASSERT(g_client.send_progress != gen, "progress generation advanced (the park cond would flip)");

    poll_waiter_list_unregister(&pw1);
    poll_waiter_list_unregister(&pw2);
    g_client.send_waiters = 0;

    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}
