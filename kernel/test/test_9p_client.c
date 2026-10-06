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
#include <thylacine/caps.h>       // LOOM.md 8.5.1: the DAC-override + chown-any caps
#include <thylacine/dev.h>        // dev9p.walk (the 8.5.1 second directory fid)
#include <thylacine/dev9p.h>
#include <thylacine/weft.h>       // Weft-6c: weft_binding_alloc + the zero-copy drive
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/page.h>       // pa_to_kva / page_to_pa (the buffer direct map)
#include <thylacine/proc.h>       // 8c-3 (#89): struct Proc + debug_stop_req (handoff skip)
#include <thylacine/rendez.h>
#include <thylacine/sched.h>      // sched(): yield to the SQPOLL kthread
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>    // T_S_IFDIR (the 8.5.1 directory Rgetattr)
#include <thylacine/thread.h>     // flush(5) legs: current_thread()->note_interruptible
#include <thylacine/types.h>
#include <thylacine/vivarium.h>  // flush(5) legs: a Linux sigtab row catches child_exit
#include "../../mm/slub.h"       // flush(5) legs: kzalloc a sigtab proc_free frees

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
void test_9p_client_death_hangs_up_once(void);
void test_9p_client_handoff_skips_stop_parked(void);
void test_9p_client_reader_hook_contract(void);
void test_9p_client_pump_ready_idle(void);
void test_9p_client_pump_ready_data_progresses(void);
void test_9p_client_pump_ready_chunked_frame_completes(void);
void test_9p_client_pump_ready_busy_when_reader_active(void);
void test_9p_client_pump_ready_eof_is_dead(void);
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
void test_9p_client_loom_dirmut_dac(void);
void test_9p_client_loom_dirmut_sqpoll(void);
void test_9p_client_loom_create_gid(void);
void test_9p_client_loom_cape(void);
void test_9p_client_loom_dirmut_names(void);
void test_9p_client_loom_enter_reads_every_client(void);
void test_9p_client_loom_enter_partial_set_rescans(void);
void test_9p_client_loom_sqpoll_parks_on_a_held_role(void);

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
// reader_pump_ready (demux) -> on_complete -> loom_post_cqe.
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

// Build thunk for submit_async: a Tgetattr on the fid passed via ctx -- an op
// an abandon flushes (a Tclunk is never flushed).
static int test_build_getattr(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    u32 fid = *(u32 *)ctx;
    return p9_session_send_getattr(s, out, cap, fid, P9_GETATTR_BASIC);
}

// A demuxed reply drives on_complete, which posts a CQE carrying the op's
// user_data + the mapped (success = 0) result.
void test_9p_client_async_op_posts_cqe(void) {
    struct Loom *l = loom_create(8, 16, false);
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

    int pumped = p9_client_reader_pump_ready(&g_client);   // recv Rclunk + demux
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
    struct Loom *l = loom_create(8, 16, false);
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
    int pumped = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(pumped, (int)P9_PUMP_DEAD, "pump sees the dead transport");
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
    struct Loom *l = loom_create(8, 16, false);
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
    int pumped = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(pumped, (int)P9_PUMP_DEAD, "pump returns DEAD (a control signal)");
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
    struct Loom *l = loom_create(8, 16, false);
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

// ARCH 21.10, "A death hangs up": the client asks its transport to hang up
// once, on the death's edge, however many paths later find the session dead --
// p9_client_mark_devgone reaches client_mark_dead_locked every time it is
// called, so the second call is a real second death. The control is the live
// session, which has asked for nothing, and the close is not a hangup.
void test_9p_client_death_hangs_up_once(void) {
    int open_rc = drive_client_open(&g_client, &g_loopback);
    struct p9_qid q;
    int walk = p9_client_walk_one(&g_client, 0, 6, (const u8 *)"a", 1, &q);
    u32 live = g_loopback.hangups;
    p9_client_mark_devgone(&g_client);
    u32 first = g_loopback.hangups;
    p9_client_mark_devgone(&g_client);
    u32 again = g_loopback.hangups;
    int close_rc = p9_client_close(&g_client);
    bool lb_closed = g_loopback.closed;
    u32 closed = g_loopback.hangups;
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);

    TEST_EXPECT_EQ(open_rc, 0, "handshake");
    TEST_EXPECT_EQ(walk, 0, "control: a live op");
    TEST_EXPECT_EQ(live, 0u, "control: a live session has asked for no hangup");
    TEST_EXPECT_EQ(first, 1u, "the death hangs up");
    TEST_EXPECT_EQ(again, 1u, "once: a second death asks for no more");
    TEST_EXPECT_EQ(close_rc, 0, "the dead session closes");
    TEST_ASSERT(lb_closed, "the close reached the transport");
    TEST_EXPECT_EQ(closed, 1u, "the close is not a hangup");
}

// The elected-reader handoff hands the role to a pending SYNC op and SKIPS an
// async op (which has no thread to run the reader loop). White-box: inject one
// of each into inflight[] and assert which one is flagged be_reader.
void test_9p_client_async_handoff_skips_async(void) {
    drive_client_open(&g_client, &g_loopback);

    g_handoff_async_fired = false;
    // Every hand-built rpc starts from zero: the handoff reads fields these
    // assignments do not name (`sending`), and stack garbage there would skip
    // the target.
    struct p9_rpc async_rpc = { 0 };
    async_rpc.tag = 30; async_rpc.done = false; async_rpc.dead = false;
    async_rpc.be_reader = false; async_rpc.reply_len = 0; async_rpc.reply_buf = NULL;
    async_rpc.on_complete = test_handoff_async_recorder;   // async -> must be skipped
    rendez_init(&async_rpc.rendez);

    struct p9_rpc sync_rpc = { 0 };
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

// 8c-3 (#89): the elected-reader handoff MUST skip an op whose thread is
// parked for a stop (DEBUG-FS-DESIGN 5c.6). That thread cannot run the reader
// loop, and the be_reader wakeup lands on a rendez it is not asleep on, so
// handing it the role strands every survivor. The role lands on a runnable
// survivor, or is dropped if none is pending (reader_active is already false
// -> a future survivor op self-elects). The handoff reads stop_parked, the
// parked thread's own record, and nothing of its Proc, so synthetic inflight
// rpcs drive it. The parked op sits at the LOWER tag: a handoff that ignored
// the flag would pick it.
void test_9p_client_handoff_skips_stop_parked(void) {
    drive_client_open(&g_client, &g_loopback);

    // From zero: the handoff reads fields the assignments below do not name.
    struct p9_rpc rpc_parked = { 0 };
    rpc_parked.tag = 40;
    rpc_parked.stop_parked = true;
    rendez_init(&rpc_parked.rendez);

    struct p9_rpc rpc_survivor = { 0 };
    rpc_survivor.tag = 41;
    rendez_init(&rpc_survivor.rendez);

    spin_lock(&g_client.lock);
    g_client.inflight[40] = &rpc_parked;
    g_client.inflight[41] = &rpc_survivor;
    spin_unlock(&g_client.lock);

    p9_client_handoff_reader(&g_client);
    bool skip_parked = !rpc_parked.be_reader;
    bool to_survivor = rpc_survivor.be_reader;

    // Both parked: no eligible op -> the role is dropped.
    rpc_survivor.be_reader   = false;
    rpc_survivor.stop_parked = true;
    p9_client_handoff_reader(&g_client);
    bool dropped = !rpc_parked.be_reader && !rpc_survivor.be_reader;

    // The control, one variable away: unparked, the lower tag takes it.
    rpc_parked.stop_parked = false;
    p9_client_handoff_reader(&g_client);
    bool control = rpc_parked.be_reader && !rpc_survivor.be_reader;

    // Unhook the stack rpcs before any verdict, so a failing assert leaves the
    // shared client holding no pointer into this frame.
    spin_lock(&g_client.lock);
    g_client.inflight[40] = NULL;
    g_client.inflight[41] = NULL;
    spin_unlock(&g_client.lock);

    TEST_ASSERT(skip_parked, "#89: a stop-parked op is NOT handed the reader role");
    TEST_ASSERT(to_survivor, "#89: the runnable survivor's op IS handed the reader role");
    TEST_ASSERT(dropped, "#89: every op stop-parked -> the role is dropped");
    TEST_ASSERT(control, "an unparked op at the lower tag takes the role (the control)");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

static bool g_pump_async_completed;
static s32  g_pump_async_result;
static struct p9_rpc g_pump_rpc;

static void pump_async_on_complete(struct p9_rpc *rpc, int status,
                                   struct p9_dispatch_result *dr) {
    (void)rpc; (void)dr;
    g_pump_async_result    = (s32)status;
    g_pump_async_completed = true;
}

// A fan-in waiter's hook on one client (LOOM.md 8.6). A free role over an empty
// stream files it on the transport's readiness list, and the reply's arrival
// wakes it; a free role over a staged frame files nothing (pump now). A held
// role files it on the role-waiter list only: a handoff that designates a sync
// op leaves it quiet (that op will read), one that leaves the role free and
// undesignated wakes it. A dead session refuses it. The unhook is idempotent.
void test_9p_client_reader_hook_contract(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 32, (const u8 *)"f", 1, NULL);   // bind fid 32
    struct Rendez rr;
    rendez_init(&rr);
    struct p9_reader_hook h;
    poll_waiter_init(&h.pw, &rr);

    // Free role, empty stream: the readiness list.
    int  idle_rc     = p9_client_reader_hook(&g_client, &h);
    bool idle_place  = (h.place == P9_HOOK_READY) && (h.pw.list == &g_loopback.ready_list);
    bool idle_quiet  = !h.pw.ready;
    g_pump_rpc.on_complete = pump_async_on_complete;
    g_pump_async_completed = false;
    u32 fid = 32;
    int  sub_rc      = p9_client_submit_async(&g_client, &g_pump_rpc, test_build_clunk, &fid);
    bool arrived     = h.pw.ready;   // the staged Rclunk walked the list
    p9_client_reader_unhook(&g_client, &h);
    bool idle_off    = (h.pw.list == NULL) && (h.place == P9_HOOK_NONE);

    // Free role, a frame staged: nothing filed, pump now.
    int  ready_rc    = p9_client_reader_hook(&g_client, &h);
    bool ready_none  = (h.pw.list == NULL) && (h.place == P9_HOOK_NONE);
    int  pumped      = p9_client_reader_pump_ready(&g_client);

    // Held role: the role-waiter list only.
    spin_lock(&g_client.lock);
    g_client.reader_active = true;
    spin_unlock(&g_client.lock);
    int  held_rc     = p9_client_reader_hook(&g_client, &h);
    bool held_place  = (h.place == P9_HOOK_ROLE) && (h.pw.list == &g_client.role_waiters_list);
    u32  hooked      = g_client.role_waiters;

    struct p9_rpc rpc_sync = { 0 };
    rpc_sync.tag = 42;
    rendez_init(&rpc_sync.rendez);
    spin_lock(&g_client.lock);
    g_client.inflight[42]  = &rpc_sync;
    g_client.reader_active = false;
    spin_unlock(&g_client.lock);
    p9_client_handoff_reader(&g_client);
    bool designated  = rpc_sync.be_reader;
    bool held_quiet  = !h.pw.ready;

    spin_lock(&g_client.lock);
    g_client.inflight[42] = NULL;
    spin_unlock(&g_client.lock);
    p9_client_handoff_reader(&g_client);
    bool woken       = h.pw.ready;

    p9_client_reader_unhook(&g_client, &h);
    u32  after       = g_client.role_waiters;
    p9_client_reader_unhook(&g_client, &h);
    u32  after2      = g_client.role_waiters;

    // Dead: refused, nothing filed.
    spin_lock(&g_client.lock);
    g_client.dead = true;
    spin_unlock(&g_client.lock);
    int  dead_rc     = p9_client_reader_hook(&g_client, &h);
    bool dead_none   = (h.pw.list == NULL) && (h.place == P9_HOOK_NONE);
    p9_client_reader_unhook(&g_client, &h);
    spin_lock(&g_client.lock);
    g_client.dead = false;
    spin_unlock(&g_client.lock);
    h.pw.magic = 0;

    TEST_ASSERT(idle_rc == 1 && idle_place && idle_quiet,
                "a free role over an empty stream hooks readiness (1)");
    TEST_ASSERT(sub_rc == 0 && arrived, "the reply's arrival wakes the readiness hook");
    TEST_ASSERT(idle_off, "the unhook takes it off the backend's list");
    TEST_ASSERT(ready_rc == 0 && ready_none, "a free role over a staged frame files nothing (0)");
    TEST_ASSERT(pumped == (int)P9_PUMP_PROGRESS && g_pump_async_completed,
                "and the pump reads it");
    TEST_ASSERT(held_rc == 1 && held_place && hooked == 1,
                "a held role hooks the role list only (1)");
    TEST_ASSERT(designated && held_quiet, "a handoff that designates a sync op leaves the hook quiet");
    TEST_ASSERT(woken, "a handoff leaving the role free and undesignated wakes the hook");
    TEST_ASSERT(after == 0 && after2 == 0, "the unhook drops the count once");
    TEST_ASSERT(dead_rc == -P9_E_IO && dead_none, "a dead session refuses the hook");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// The readiness-gated reader pump (LOOM.md 8.6). It reads only over a ready
// stream -- bytes or the EOF at a frame boundary -- so it never blocks there;
// an empty loopback models a blocking recv and is not ready.
// =============================================================================

// Nothing to read: IDLE, the stream untouched, the session alive and usable.
// The pre-10-06 pump read blind here, and an empty loopback's recv is the EOF
// that latches the session dead.
void test_9p_client_pump_ready_idle(void) {
    drive_client_open(&g_client, &g_loopback);   // handshake drains the loopback

    int r = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_IDLE, "empty stream -> IDLE");
    TEST_ASSERT(!g_client.dead, "IDLE must NOT mark the session dead");
    TEST_ASSERT(!g_client.reader_active, "IDLE leaves the role free");
    TEST_ASSERT(p9_client_is_open(&g_client), "session still open after IDLE");

    int wrc = p9_client_walk_one(&g_client, 0, 31, (const u8 *)"f", 1, NULL);
    TEST_EXPECT_EQ(wrc, 0, "walk succeeds after IDLE (session reusable)");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// A reply on the wire: the pump demuxes it -> PROGRESS.
void test_9p_client_pump_ready_data_progresses(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 32, (const u8 *)"f", 1, NULL);   // bind fid 32

    g_pump_rpc.on_complete = pump_async_on_complete;
    g_pump_async_completed = false;
    g_pump_async_result    = 0x7fffffff;
    u32 fid = 32;
    int rc = p9_client_submit_async(&g_client, &g_pump_rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async stages an Rclunk on the wire");

    int r = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_PROGRESS, "data ready -> PROGRESS");
    TEST_ASSERT(g_pump_async_completed, "on_complete fired");
    TEST_EXPECT_EQ(g_pump_async_result, 0, "clunk success -> result 0");
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), (int)P9_PUMP_IDLE,
                   "and the drained stream is IDLE again");

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Frame atomicity: a frame delivered in sub-header chunks is assembled whole.
void test_9p_client_pump_ready_chunked_frame_completes(void) {
    drive_client_open(&g_client, &g_loopback);
    p9_client_walk_one(&g_client, 0, 33, (const u8 *)"f", 1, NULL);

    p9_loopback_set_chunk_size(&g_loopback, 3);   // < the 7-byte Rclunk frame

    g_pump_rpc.on_complete = pump_async_on_complete;
    g_pump_async_completed = false;
    u32 fid = 33;
    int rc = p9_client_submit_async(&g_client, &g_pump_rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async stages a chunked Rclunk");

    int r = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_PROGRESS, "chunked frame -> PROGRESS");
    TEST_ASSERT(g_pump_async_completed, "on_complete fired for the aggregated frame");

    p9_loopback_set_chunk_size(&g_loopback, 0);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// Another thread already holds the reader role: the pump defers (P9_PUMP_BUSY)
// without touching the stream. White-box: set reader_active directly.
void test_9p_client_pump_ready_busy_when_reader_active(void) {
    drive_client_open(&g_client, &g_loopback);

    g_client.reader_active = true;     // simulate a concurrent elected reader
    int r = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_BUSY, "reader already active -> BUSY (no-op)");
    TEST_ASSERT(!g_client.dead, "BUSY must not mark the session dead");
    g_client.reader_active = false;    // release so destroy is clean

    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// The peer's EOF is readiness too: the pump reads it, and the session dies.
void test_9p_client_pump_ready_eof_is_dead(void) {
    drive_client_open(&g_client, &g_loopback);

    p9_loopback_force_eof(&g_loopback);
    int r = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(r, (int)P9_PUMP_DEAD, "the EOF -> DEAD");
    TEST_ASSERT(g_client.dead, "the EOF marks the session dead");
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), (int)P9_PUMP_DEAD,
                   "and a dead session stays DEAD");

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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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
    int pumped = p9_client_reader_pump_ready(&g_client);
    TEST_EXPECT_EQ(pumped, (int)P9_PUMP_PROGRESS, "the late Rflush drained ownerless (no UAF)");

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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(2, 2, false);   // cq_entries = 2 (smallest exercising the hold)
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(2, 2, false);   // cq=2 forces the multishot to back-pressure
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
    struct Burrow *b = burrow_create_anon(len, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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

    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
static u64 g_loom_msetattr_size;
static u64 loom_rd_le64(const u8 *p) {
    u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8 * i); return v;
}
static u32 loom_rd_le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 loom_rd_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void loom_cap_name(u8 *dst, u32 *dlen, const u8 *src, u32 n) {
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
    *dlen = n;
}
// LOOM.md 8.5.1 fixtures -- the parent-directory check reads the Rgetattr below.
// With g_loom_ga_on, every Tgetattr answers a DIRECTORY of g_loom_ga_mode owned
// by g_loom_ga_uid:g_loom_ga_gid, except fid g_loom_ga_deny_fid, which answers a
// 0700 directory owned by LOOM_GA_OTHER_UID (writable by no identity under test).
// g_loom_ga_fail answers every Tgetattr Rlerror (the no-stat leg). The wire side:
// g_loom_wire_mut counts child-mutation T-messages that REACHED the server, and
// g_loom_wire_gid is the gid the last Tmkdir / Tsymlink / Tmknod carried, and
// g_loom_wire_mode the mode the last Tmkdir / Tmknod carried.
#define LOOM_GA_OTHER_UID 0xAAAAu
static bool g_loom_ga_on, g_loom_ga_fail;
static u32  g_loom_ga_mode, g_loom_ga_uid, g_loom_ga_gid;
static u64  g_loom_ga_valid_clear;   // valid bits the Rgetattr leaves out
static u32  g_loom_ga_deny_fid = P9_NOFID;
static u32  g_loom_wire_mut, g_loom_wire_gid, g_loom_wire_mode;
static void loom_ga_reset(void) {
    g_loom_ga_on = g_loom_ga_fail = false;
    g_loom_ga_mode = g_loom_ga_uid = g_loom_ga_gid = 0;
    g_loom_ga_valid_clear = 0;
    g_loom_ga_deny_fid = P9_NOFID;
    g_loom_wire_mut = 0;
    g_loom_wire_gid = 0xDEADBEEFu;
    g_loom_wire_mode = 0xDEADBEEFu;
}
static void loom_ga_dir(u32 mode, u32 uid, u32 gid) {
    g_loom_ga_on = true;
    g_loom_ga_mode = mode; g_loom_ga_uid = uid; g_loom_ga_gid = gid;
}
static void loom_wr_le32(u8 *p, u32 v) {
    for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i));
}
// Bind a fresh identity as the Loom's 8.5.1 submitter (sys_loom_setup does
// this for a real ring; loom_create alone leaves it NULL). Free the Loom first.
static struct Proc *loom_test_ident(struct Loom *l, u32 principal, u32 gid,
                                    caps_t caps) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->principal_id = principal;
    p->primary_gid  = gid;
    p->caps         = caps;
    l->ident        = p;
    l->ident_pid    = p->pid;
    return p;
}
static void loom_test_ident_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}
// The gid a create T-message carries, or 0xDEADBEEF when the frame is short.
// Layouts: Tmkdir dfid name mode gid; Tsymlink fid name symtgt gid; Tmknod dfid
// name mode major minor gid (9P2000.L).
static u32 loom_wire_create_gid(u8 type, const u8 *req, size_t req_len) {
    size_t off = (size_t)P9_HDR_LEN + 4;
    if (req_len < off + 2) return 0xDEADBEEFu;
    off += 2 + loom_rd_le16(req + off);
    if (type == P9_TSYMLINK) {
        if (req_len < off + 2) return 0xDEADBEEFu;
        off += 2 + loom_rd_le16(req + off);
    } else {
        off += (type == P9_TMKNOD) ? 12 : 4;
    }
    return req_len >= off + 4 ? loom_rd_le32(req + off) : 0xDEADBEEFu;
}

// The mode a Tmkdir / Tmknod carries: the first field after the name.
static u32 loom_wire_create_mode(const u8 *req, size_t req_len) {
    size_t off = (size_t)P9_HDR_LEN + 4;
    if (req_len < off + 2) return 0xDEADBEEFu;
    off += 2 + loom_rd_le16(req + off);
    return req_len >= off + 4 ? loom_rd_le32(req + off) : 0xDEADBEEFu;
}

static int loom_mut_capture_responder(void *ctx, const u8 *req, size_t req_len,
                                      u8 *resp, size_t cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) >= 0) {
        if (type == P9_TMKDIR || type == P9_TMKNOD || type == P9_TSYMLINK ||
            type == P9_TUNLINKAT || type == P9_TRENAMEAT || type == P9_TLINK)
            g_loom_wire_mut++;
        if (type == P9_TMKDIR || type == P9_TMKNOD || type == P9_TSYMLINK)
            g_loom_wire_gid = loom_wire_create_gid(type, req, req_len);
        if (type == P9_TMKDIR || type == P9_TMKNOD)
            g_loom_wire_mode = loom_wire_create_mode(req, req_len);
        if (type == P9_TGETATTR && g_loom_ga_fail)
            return rlerror_responder(ctx, req, req_len, resp, cap);
        if (type == P9_TGETATTR && g_loom_ga_on && req_len >= (size_t)P9_HDR_LEN + 4) {
            int n = canonical_responder(ctx, req, req_len, resp, cap);
            if (n < (int)P9_HDR_LEN + 33) return n;
            bool deny = loom_rd_le32(req + P9_HDR_LEN) == g_loom_ga_deny_fid;
            resp[P9_HDR_LEN + 8] = P9_QTDIR;                        // qid.type
            loom_wr_le32(resp + P9_HDR_LEN + 21,                    // mode
                         T_S_IFDIR | (deny ? 0700u : g_loom_ga_mode));
            loom_wr_le32(resp + P9_HDR_LEN + 25, deny ? LOOM_GA_OTHER_UID : g_loom_ga_uid);
            loom_wr_le32(resp + P9_HDR_LEN + 29, deny ? LOOM_GA_OTHER_UID : g_loom_ga_gid);
            u64 v = loom_rd_le64(resp + P9_HDR_LEN) & ~g_loom_ga_valid_clear;
            for (int i = 0; i < 8; i++) resp[P9_HDR_LEN + i] = (u8)(v >> (8 * i));
            return n;
        }
        if (type == P9_TMKDIR && req_len >= (size_t)P9_HDR_LEN + 6) {
            u32 nl = loom_rd_le16(req + P9_HDR_LEN + 4);              // name s len
            if (req_len >= (size_t)P9_HDR_LEN + 6 + nl + 4) {
                loom_cap_name(g_loom_mname, &g_loom_mname_len, req + P9_HDR_LEN + 6, nl);
                g_loom_mname_mode = loom_rd_le32(req + P9_HDR_LEN + 6 + nl);  // mode after name
            }
        } else if (type == P9_TSETATTR && req_len >= (size_t)P9_HDR_LEN + 28) {
            g_loom_msetattr_valid = loom_rd_le32(req + P9_HDR_LEN + 4);   // valid u32
            g_loom_msetattr_mode  = loom_rd_le32(req + P9_HDR_LEN + 8);   // mode u32
            g_loom_msetattr_size  = loom_rd_le64(req + P9_HDR_LEN + 20);  // size u64
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
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);   // the creator's own dir (8.5.1 passes)
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
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // create requires RIGHT_WRITE on the dir
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write dir handle");
    struct Proc *who = loom_test_ident(l, 0x1234u, 0x5678u, 0);
    TEST_ASSERT(who != NULL, "bind the creator identity");

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
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x5678u,
                   "SQE gid 0 -> the creator's primary group on the wire (8.5.1)");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "op reaped");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "op buffer pin balanced");

    burrow_unref(b);
    loom_unref(l);
    loom_test_ident_drop(who);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// SETATTR end-to-end: the input struct p9_setattr is read FROM the pinned buffer
// (align-safe copy); the capture responder proves valid + mode reached the wire;
// Rsetattr (empty) -> result 0. Exercises the struct-from-buffer input path.
// Scan the CQ ring for the CQE echoing `ud` and return its result. The Loom
// completions land in submit-completion order, NOT submit order: an inline
// reject completes at submit while an async op completes when its reply lands,
// so a mixed batch (reject + async + reject) posts CQEs out of leg order.
// Match by user_data instead of assuming index == leg.
static s32 loom_cqe_result(const struct loom_cqe *cqes,
                           const struct loom_ring_hdr *h, u64 ud) {
    u32 tail = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < tail && i < 64; i++)
        if (cqes[i].user_data == ud) return cqes[i].result;
    return 0x7FFFFFFF;   // sentinel: not found (never a valid < 0 or == 0 result)
}

void test_9p_client_loom_setattr_e2e(void) {
    g_loom_msetattr_valid = 0; g_loom_msetattr_mode = 0;
    loom_ga_reset();
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
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;          // setattr mutates metadata -> RIGHT_WRITE
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register write handle");

    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    int hc0 = burrow_handle_count(b);
    struct p9_setattr *sa = (struct p9_setattr *)bkva;

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // (a) T_WSTAT_SIZE audit F2: a Loom SETATTR carrying an IDENTITY axis
    // (MODE/UID/GID) is REJECTED fail-closed -- the async submit cannot run the
    // owner-only perm_wstat_check the sync path enforces, and a hollow-RIGHT_WRITE
    // O_PATH handle would otherwise chmod/chown any X-reachable file. The chmod
    // must NEVER reach the wire (pre-fix this landed a Tsetattr(MODE=0600)).
    g_loom_msetattr_valid = 0; g_loom_msetattr_mode = 0; g_loom_msetattr_size = 0;
    for (u32 i = 0; i < sizeof(*sa); i++) ((u8 *)sa)[i] = 0;
    sa->valid = P9_SETATTR_MODE; sa->mode = 0600u;
    cl_stage_mut(l, 0, LOOM_OP_SETATTR, /*handle=*/0, /*offset=*/0,
                 /*len=*/(u32)sizeof(struct p9_setattr), /*bidx=*/0, /*buf_off=*/0,
                 0, 0, 0, 0x5E77000000000000ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (chmod)");
    TEST_ASSERT(loom_cqe_result(cqes, h, 0x5E77000000000000ULL) < 0,
                "async chmod rejected fail-closed (F2)");
    TEST_EXPECT_EQ((u64)g_loom_msetattr_valid, (u64)0, "chmod NEVER reached the wire");
    // Drain the CQ so the NEXT leg's min_complete=1 waits for its OWN new CQE
    // (an inline reject + an async op mixed in one un-drained CQ makes a bare
    // min_complete=1 return on a stale entry before the async op lands).
    __atomic_store_n(&h->cq_head, __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);

    // (b) SIZE (truncate) on the SAME RIGHT_WRITE (non-O_PATH) handle is the
    // legitimate content mutation -- its authority IS the write-fd -- so it
    // reaches the wire full-width (a >4 GiB value proves the u64 path).
    for (u32 i = 0; i < sizeof(*sa); i++) ((u8 *)sa)[i] = 0;
    sa->valid = P9_SETATTR_SIZE; sa->size = 0x112233445566ull;
    cl_stage_mut(l, 1, LOOM_OP_SETATTR, /*handle=*/0, /*offset=*/0,
                 /*len=*/(u32)sizeof(struct p9_setattr), /*bidx=*/0, /*buf_off=*/0,
                 0, 0, 0, 0x5E77000000000001ULL);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);
    n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (truncate)");
    TEST_EXPECT_EQ((u64)(s64)loom_cqe_result(cqes, h, 0x5E77000000000001ULL), (u64)0,
                   "truncate result = 0 (scalar success)");
    TEST_EXPECT_EQ((u64)g_loom_msetattr_valid, (u64)P9_SETATTR_SIZE, "truncate valid on wire");
    TEST_EXPECT_EQ(g_loom_msetattr_size, 0x112233445566ull, "truncate size is the full u64");
    __atomic_store_n(&h->cq_head, __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);

    // (c) T_WSTAT_SIZE audit F1 [P0]: SIZE (truncate) through an O_PATH
    // (CWALKONLY) handle is REJECTED -- the O_PATH handle is born RIGHT_WRITE but
    // perm_check-EXEMPT, so its RIGHT_WRITE is hollow (the async twin of the sync
    // O_PATH truncate bypass). Flag the registered Spoor CWALKONLY + re-submit
    // SIZE; it must be rejected and NEVER reach the wire.
    sp->flag |= CWALKONLY;
    g_loom_msetattr_valid = 0; g_loom_msetattr_size = 0;
    for (u32 i = 0; i < sizeof(*sa); i++) ((u8 *)sa)[i] = 0;
    sa->valid = P9_SETATTR_SIZE; sa->size = 16;
    cl_stage_mut(l, 2, LOOM_OP_SETATTR, /*handle=*/0, /*offset=*/0,
                 /*len=*/(u32)sizeof(struct p9_setattr), /*bidx=*/0, /*buf_off=*/0,
                 0, 0, 0, 0x5E77000000000002ULL);
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);
    n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (O_PATH truncate)");
    TEST_ASSERT(loom_cqe_result(cqes, h, 0x5E77000000000002ULL) < 0,
                "truncate via O_PATH handle rejected (F1 P0)");
    TEST_EXPECT_EQ((u64)g_loom_msetattr_valid, (u64)0, "O_PATH truncate NEVER reached the wire");

    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "ops reaped");
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
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);   // both dirs the creator's own (8.5.1)
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
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    rights_t rt = RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register olddir (slot 0)");
    loom_install_test_handle(l, 1, sp, RIGHT_WRITE);   // newdir (slot 1; same Spoor)
    struct Proc *who = loom_test_ident(l, 0x1234u, 0x5678u, 0);
    TEST_ASSERT(who != NULL, "bind the creator identity");

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
    loom_test_ident_drop(who);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// LOOM.md 8.5.1: the directory-mutation authority. The six child-mutation ops
// run the sync twins' parent W|X perm_check against the creator's LIVE identity
// (l->ident), an SQPOLL ring refuses them, and the create ops' SQE gid resolves
// to one of the creator's own groups. Every refusal is inline and puts NOTHING on
// the wire (g_loom_wire_mut is the server's own count, not the kernel's).
// =============================================================================

// The capture-responder client, opened the way mkdir_e2e opens it; returns the
// root Spoor holding the attach's one ref.
static struct Spoor *loom_mut_open(void) {
    if (p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                         loom_mut_capture_responder, NULL) != 0) return NULL;
    if (p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                       p9_loopback_ops_for(&g_loopback), g_recv_buf,
                       sizeof(g_recv_buf)) != 0) return NULL;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    if (p9_client_handshake(&g_client, uname, sizeof(uname), aname,
                            sizeof(aname), 0) != 0) return NULL;
    return dev9p_attach_client(&g_client, 0);
}

// A second directory Spoor with its own fid (a plain Twalk: no Twalkgetattr, so
// the client never latches cacheable and every stat below is a live Tgetattr).
static struct Spoor *loom_walk_sub(struct Spoor *root, u32 *out_fid) {
    const char *name = "sub";
    struct Spoor *nc = spoor_clone(root);
    if (!nc) return NULL;
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    if (!w) { spoor_clunk(nc); return NULL; }
    walkqid_free(w);
    struct p9_client *cl; u32 fid;
    if (dev9p_client_fid(nc, &cl, &fid) != 0) { spoor_clunk(nc); return NULL; }
    *out_fid = fid;
    return nc;
}

// Submit ONE mutation SQE at the ring's next slot (handle 0 = the directory; r3 =
// the second handle), wait for it, reap its CQE, and return the result -- so a
// long leg table never fills the CQ and each leg reads exactly its own CQE.
static s32 loom_dm_submit(struct Loom *l, u8 opcode, u32 len, u64 offset,
                          u64 r1, u64 r2, u64 r3) {
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 tail = __atomic_load_n(&h->sq_tail, __ATOMIC_ACQUIRE);
    cl_stage_mut(l, tail & h->sq_mask, opcode, /*handle=*/0, offset, len,
                 /*bidx=*/0, /*buf_off=*/0, r1, r2, r3,
                 0x8510000000000000ULL | (u64)tail);
    __atomic_store_n(&h->sq_tail, tail + 1u, __ATOMIC_RELEASE);
    u32 ct = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE);
    (void)loom_enter(l, 1, 1, 0);
    if (__atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) == ct) return 0x7FFFFFFF;
    s32 r = cqes[ct & h->cq_mask].result;
    __atomic_store_n(&h->cq_head, ct + 1u, __ATOMIC_RELEASE);
    return r;
}

// One op, one identity: expect `want`, and expect the op on the wire iff want == 0.
static void loom_dm_leg(struct Loom *l, u8 opcode, u32 len, u64 offset, u64 r1,
                        u64 r2, u64 r3, s32 want, const char *what) {
    u32 before = g_loom_wire_mut;
    s32 got = loom_dm_submit(l, opcode, len, offset, r1, r2, r3);
    TEST_EXPECT_EQ((u64)(s64)got, (u64)(s64)want, what);
    TEST_EXPECT_EQ((u64)(g_loom_wire_mut - before), (u64)(want == 0 ? 1u : 0u), what);
}

// The six ops' SQE shapes over region "abcdef": a one-name op names "abc"; a
// two-name op splits [0,3) ++ [3,6). gid 0 throughout (the creator's primary).
#define DM_MKDIR(l, w, s)    loom_dm_leg(l, LOOM_OP_MKDIR,    3, 0, 0755u, 0, 0, w, s)
#define DM_MKNOD(l, w, s)    loom_dm_leg(l, LOOM_OP_MKNOD,    3, 0, 0010644u, 0, 0, w, s)
#define DM_SYMLINK(l, w, s)  loom_dm_leg(l, LOOM_OP_SYMLINK,  6, 0, 3, 0, 0, w, s)
#define DM_UNLINKAT(l, w, s) loom_dm_leg(l, LOOM_OP_UNLINKAT, 3, 0, 0, 0, 0, w, s)
#define DM_RENAMEAT(l, w, s) loom_dm_leg(l, LOOM_OP_RENAMEAT, 6, 0, 3, 0, 1, w, s)
#define DM_LINK(l, w, s)     loom_dm_leg(l, LOOM_OP_LINK,     3, 0, 0, 0, 1, w, s)

// The truth table. Directory: 0755 owned by 0x1234:0x5678. Handle 0 = that dir;
// handle 1 = a second dir (RENAMEAT's newdir, LINK's source). Every leg's control
// is the leg beside it: the same op, one identity bit apart.
void test_9p_client_loom_dirmut_dac(void) {
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    struct Spoor *root = loom_mut_open();
    TEST_ASSERT(root != NULL, "capture client + root");
    u32 sub_fid = P9_NOFID;
    struct Spoor *sub = loom_walk_sub(root, &sub_fid);
    TEST_ASSERT(sub != NULL, "a second directory Spoor");
    struct p9_client *root_cl; u32 root_fid = P9_NOFID;
    TEST_ASSERT(dev9p_client_fid(root, &root_cl, &root_fid) == 0 && root_fid != sub_fid,
                "the two directories carry distinct fids");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    // O_PATH-shaped rights: R|W, the hollow RIGHT_WRITE this gate exists for.
    loom_install_test_handle(l, 0, root, RIGHT_READ | RIGHT_WRITE);
    loom_install_test_handle(l, 1, sub,  RIGHT_READ | RIGHT_WRITE);
    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    const char *nm = "abcdef";
    for (u32 i = 0; i < 6; i++) bkva[i] = (u8)nm[i];

    // (1) No identity (a kernel-internal Loom): every op fails closed.
    DM_MKDIR(l, -(s32)T_E_ACCES, "no identity bound -> MKDIR -EACCES, nothing on the wire");

    // (2) A stranger (other bits r-x): each op refused, never on the wire.
    struct Proc *who = loom_test_ident(l, 0x9999u, 0x9999u, 0);
    TEST_ASSERT(who != NULL, "bind a stranger");
    DM_MKDIR(l,    -(s32)T_E_ACCES, "stranger MKDIR -> -EACCES");
    DM_MKNOD(l,    -(s32)T_E_ACCES, "stranger MKNOD -> -EACCES");
    DM_SYMLINK(l,  -(s32)T_E_ACCES, "stranger SYMLINK -> -EACCES");
    DM_UNLINKAT(l, -(s32)T_E_ACCES, "stranger UNLINKAT -> -EACCES");
    DM_RENAMEAT(l, -(s32)T_E_ACCES, "stranger RENAMEAT -> -EACCES");
    DM_LINK(l,     -(s32)T_E_ACCES, "stranger LINK -> -EACCES");

    // (3) The owner (owner bits rwx): each op reaches the wire.
    who->principal_id = 0x1234u; who->primary_gid = 0x5678u;
    DM_MKDIR(l,    0, "owner MKDIR reaches the wire");
    DM_MKNOD(l,    0, "owner MKNOD reaches the wire");
    DM_SYMLINK(l,  0, "owner SYMLINK reaches the wire");
    DM_UNLINKAT(l, 0, "owner UNLINKAT reaches the wire");
    DM_RENAMEAT(l, 0, "owner RENAMEAT reaches the wire");
    DM_LINK(l,     0, "owner LINK reaches the wire");

    // (4) W and X are both required, owner-first: an owner with rw- is refused
    // even though group/other would not help it (owner bits only).
    loom_ga_dir(0677u, 0x1234u, 0x5678u);
    DM_MKDIR(l, -(s32)T_E_ACCES, "owner without X (rw-) -> -EACCES");
    loom_ga_dir(0577u, 0x1234u, 0x5678u);
    DM_MKDIR(l, -(s32)T_E_ACCES, "owner without W (r-x) -> -EACCES (owner-first)");

    // (5) Group: a member is judged on the group triple.
    who->principal_id = 0x7777u;                     // primary 0x5678 = the dir's group
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    DM_MKDIR(l, -(s32)T_E_ACCES, "group member, group r-x -> -EACCES");
    loom_ga_dir(0775u, 0x1234u, 0x5678u);
    DM_MKDIR(l, 0, "group member, group rwx -> reaches the wire");

    // (6) The DAC override is a capability (I-22), read live: a stranger holding
    // CAP_HOSTOWNER or CAP_DAC_OVERRIDE passes; the same stranger without it does not.
    who->principal_id = 0x9999u; who->primary_gid = 0x9999u;
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    who->caps = CAP_HOSTOWNER;
    DM_MKDIR(l, 0, "stranger + CAP_HOSTOWNER -> reaches the wire");
    who->caps = CAP_DAC_OVERRIDE;
    DM_MKDIR(l, 0, "stranger + CAP_DAC_OVERRIDE -> reaches the wire");
    who->caps = 0;
    DM_MKDIR(l, -(s32)T_E_ACCES, "the same stranger, caps dropped -> -EACCES (read live)");

    // (7) RENAMEAT checks BOTH directories; LINK checks only where the link lands.
    who->principal_id = 0x1234u; who->primary_gid = 0x5678u;
    g_loom_ga_deny_fid = sub_fid;
    DM_RENAMEAT(l, -(s32)T_E_ACCES, "RENAMEAT, newdir unwritable -> -EACCES");
    DM_LINK(l, 0, "LINK whose SOURCE sits in an unwritable dir -> reaches the wire");
    g_loom_ga_deny_fid = root_fid;
    DM_RENAMEAT(l, -(s32)T_E_ACCES, "RENAMEAT, olddir unwritable -> -EACCES");
    DM_LINK(l, -(s32)T_E_ACCES, "LINK into an unwritable dir -> -EACCES");
    g_loom_ga_deny_fid = P9_NOFID;

    // (8) No stat, no op: a Tgetattr the server refuses fails closed.
    g_loom_ga_fail = true;
    DM_UNLINKAT(l, -(s32)T_E_IO, "parent stat refused -> -EIO, nothing on the wire");
    g_loom_ga_fail = false;

    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "every op reaped");
    burrow_unref(b);
    loom_unref(l);
    spoor_clunk(sub);
    spoor_clunk(root);
    loom_test_ident_drop(who);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// An SQPOLL ring refuses the six ops (-EOPNOTSUPP): its kthread submits, and the
// parent stat may be a wire RPC a hung server never answers -- the kthread would
// then never reach its stop flag and loom_free's join would hang the owner's
// exit. The identity is the creator's own and the dir its own 0755 dir, so the
// refusal is the ring's mode, not the DAC.
void test_9p_client_loom_dirmut_sqpoll(void) {
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    struct Spoor *root = loom_mut_open();
    TEST_ASSERT(root != NULL, "capture client + root");
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->principal_id = 0x1234u; p->primary_gid = 0x5678u; p->caps = 0;

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd), 0,
                   "SQPOLL setup");
    struct Handle hh;
    TEST_ASSERT(handle_get(p, fd, &hh) == 0, "handle_get(loom fd)");
    struct Loom *l = (struct Loom *)hh.obj;
    TEST_ASSERT(l->sqpoll != NULL, "SQPOLL kthread spawned");
    TEST_ASSERT(l->ident == p && l->ident_pid == p->pid,
                "setup bound the creator as the 8.5.1 identity");
    loom_install_test_handle(l, 0, root, RIGHT_READ | RIGHT_WRITE);
    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    bkva[0] = 'a'; bkva[1] = 'b'; bkva[2] = 'c';

    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    cl_stage_mut(l, 0, LOOM_OP_MKDIR, 0, 0, /*len=*/3, /*bidx=*/0, 0, 0755u, 0, 0,
                 0x5A00000000000001ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0,
                   "ENTER on SQPOLL only wakes the kthread");
    bool posted = false;
    for (u32 round = 0; round < 100000u; round++) {
        if (__atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) >= 1u) { posted = true; break; }
        sched();
    }
    TEST_ASSERT(posted, "the kthread posted the MKDIR's CQE");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_OPNOTSUPP),
                   "SQPOLL MKDIR -> -EOPNOTSUPP");
    TEST_EXPECT_EQ((u64)g_loom_wire_mut, (u64)0, "nothing reached the wire");

    handle_put(&hh);
    burrow_unref(b);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);                                    // last handle -> loom_free joins
    spoor_clunk(root);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// The create ops' SQE gid: 0 is the creator's primary group; any other value must
// be a group the creator is in unless it holds chown-any authority (CAP_HOSTOWNER
// or CAP_CHOWN) -- perm_wstat_check's chgrp rule applied at birth. The wire gid is
// the RESOLVED one. MKDIR/SYMLINK carry it in _resv1[2], MKNOD in `offset`.
void test_9p_client_loom_create_gid(void) {
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    struct Spoor *root = loom_mut_open();
    TEST_ASSERT(root != NULL, "capture client + root");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    loom_install_test_handle(l, 0, root, RIGHT_READ | RIGHT_WRITE);
    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    const char *nm = "abcdef";
    for (u32 i = 0; i < 6; i++) bkva[i] = (u8)nm[i];
    struct Proc *who = loom_test_ident(l, 0x1234u, 0x5678u, 0);
    TEST_ASSERT(who != NULL, "bind the creator");
    who->supp_gids[0] = 0x4444u;
    who->supp_gid_count = 1;

    // MKDIR (gid in _resv1[2]).
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0, 0), (u64)0, "gid 0");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x5678u, "gid 0 -> the primary group on the wire");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x4444u, 0), (u64)0,
                   "a supplementary group");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x4444u, "the supplementary group on the wire");
    g_loom_wire_gid = 0xDEADBEEFu;
    loom_dm_leg(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x3333u, 0, -(s32)T_E_ACCES,
                "MKDIR into a group the creator is not in -> -EACCES");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0xDEADBEEFu, "no create carried the refused gid");
    loom_dm_leg(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x100005678ull, 0, -(s32)T_E_INVAL,
                "a gid wider than u32 -> -EINVAL (never truncated onto the wire)");
    who->caps = CAP_CHOWN;
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x3333u, 0), (u64)0,
                   "CAP_CHOWN: any group");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x3333u, "the chosen group on the wire");
    who->caps = CAP_HOSTOWNER;
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x3434u, 0), (u64)0,
                   "CAP_HOSTOWNER: any group");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x3434u, "the chosen group on the wire");
    who->caps = 0;

    // SYMLINK (gid in _resv1[2]; _resv1[1] is the name split).
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_SYMLINK, 6, 0, 3, 0, 0), (u64)0, "SYMLINK gid 0");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x5678u, "SYMLINK: the primary group on the wire");
    loom_dm_leg(l, LOOM_OP_SYMLINK, 6, 0, 3, 0x3333u, 0, -(s32)T_E_ACCES,
                "SYMLINK into a foreign group -> -EACCES");

    // MKNOD (gid in `offset`; _resv1[1..3] are mode/major/minor).
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKNOD, 3, 0, 0010644u, 0, 0), (u64)0,
                   "MKNOD gid 0");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x5678u, "MKNOD: the primary group on the wire");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKNOD, 3, 0x4444u, 0010644u, 0, 0), (u64)0,
                   "MKNOD into a supplementary group");
    TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0x4444u, "MKNOD: the supplementary group on the wire");
    loom_dm_leg(l, LOOM_OP_MKNOD, 3, 0x3333u, 0010644u, 0, 0, -(s32)T_E_ACCES,
                "MKNOD into a foreign group -> -EACCES");

    // The create mode: rwx only, as the sync create sends it. The 0755 legs are
    // the control one variable away -- a plain mode crosses unchanged.
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0, 0), (u64)0, "MKDIR 0755");
    TEST_EXPECT_EQ((u64)g_loom_wire_mode, (u64)0755u, "MKDIR: a plain mode crosses unchanged");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 07777u, 0, 0), (u64)0, "MKDIR 07777");
    TEST_EXPECT_EQ((u64)g_loom_wire_mode, (u64)0777u,
                   "MKDIR: setuid/setgid/sticky never reach the wire");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKDIR, 3, 0, 0x10000000ull | 0755u, 0, 0), (u64)0,
                   "MKDIR with high garbage");
    TEST_EXPECT_EQ((u64)g_loom_wire_mode, (u64)0755u, "MKDIR: bits past 0777 never reach the wire");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKNOD, 3, 0, 0010755u, 0, 0), (u64)0, "MKNOD 0755");
    TEST_EXPECT_EQ((u64)g_loom_wire_mode, (u64)0010755u, "MKNOD: the type + a plain mode cross");
    TEST_EXPECT_EQ((u64)loom_dm_submit(l, LOOM_OP_MKNOD, 3, 0, 0016755u, 0, 0), (u64)0, "MKNOD 06755");
    TEST_EXPECT_EQ((u64)g_loom_wire_mode, (u64)0010755u,
                   "MKNOD: the type survives, setuid/setgid/sticky do not");

    burrow_unref(b);
    loom_unref(l);
    spoor_clunk(root);
    loom_test_ident_drop(who);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// One GETATTR on handle 0 into buffer 0 at `off`; returns the CQE result.
static s32 loom_ga_submit(struct Loom *l, u64 off) {
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 tail = __atomic_load_n(&h->sq_tail, __ATOMIC_ACQUIRE);
    cl_stage_rw(l, tail & h->sq_mask, LOOM_OP_GETATTR, /*handle=*/0, P9_GETATTR_BASIC,
                (u32)sizeof(struct p9_attr), /*bidx=*/0, off, 0x6A77000000000000ULL | tail);
    __atomic_store_n(&h->sq_tail, tail + 1u, __ATOMIC_RELEASE);
    u32 ct = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE);
    (void)loom_enter(l, 1, 1, 0);
    if (__atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) == ct) return 0x7FFFFFFF;
    s32 r = cqes[ct & h->cq_mask].result;
    __atomic_store_n(&h->cq_head, ct + 1u, __ATOMIC_RELEASE);
    return r;
}

// The identity cape (IDENTITY-DESIGN 3.2) on the Loom surface. The directory is
// the operator's failing case: a private 0700 one owned by a HOST's ids (501:20,
// a Mac's). Uncaped, it refuses the guest creator and GETATTR reports the host's
// owner; caped, the parent check reads the caped stat, a create sends P9_NOGID
// and refuses a named group (a chgrp, which the cape refuses), and GETATTR hands
// userspace the caped owner, marked valid even when the server left it out.
void test_9p_client_loom_cape(void) {
    for (int caped = 0; caped <= 1; caped++) {
        loom_ga_reset();
        loom_ga_dir(0700u, 501u, 20u);
        struct Spoor *root = loom_mut_open();
        TEST_ASSERT(root != NULL, "capture client + root");
        if (caped) p9_client_set_cape(&g_client, 0x1234u, 0x5678u);   // before any stat
        struct Loom *l = loom_create(8, 16, false);
        TEST_ASSERT(l != NULL, "loom_create");
        loom_install_test_handle(l, 0, root, RIGHT_READ | RIGHT_WRITE);
        struct Burrow *b; u8 *bkva;
        loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
        const char *nm = "abcdef";
        for (u32 i = 0; i < 6; i++) bkva[i] = (u8)nm[i];
        struct Proc *who = loom_test_ident(l, 0x1234u, 0x5678u, 0);
        TEST_ASSERT(who != NULL, "bind the creator (the mounter)");
        struct p9_attr *a = (struct p9_attr *)(bkva + 512);

        if (!caped) {
            DM_MKDIR(l, -(s32)T_E_ACCES, "uncaped: a host-owned 0700 dir refuses the guest");
            TEST_EXPECT_EQ((u64)loom_ga_submit(l, 512), (u64)sizeof(struct p9_attr),
                           "uncaped GETATTR");
            TEST_EXPECT_EQ((u64)a->uid, (u64)501u, "uncaped GETATTR: the host's uid");
            TEST_EXPECT_EQ((u64)a->gid, (u64)20u, "uncaped GETATTR: the host's gid");
        } else {
            DM_MKDIR(l, 0, "caped: the mounter owns the dir");
            TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)P9_NOGID,
                           "caped MKDIR: the server keeps its own group");
            g_loom_wire_gid = 0xDEADBEEFu;
            loom_dm_leg(l, LOOM_OP_MKDIR, 3, 0, 0755u, 0x5678u, 0, -(s32)T_E_ACCES,
                        "caped: naming a group, even the primary, is a refused chgrp");
            TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)0xDEADBEEFu, "no create carried it");
            DM_SYMLINK(l, 0, "caped SYMLINK");
            TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)P9_NOGID, "caped SYMLINK: gid (u32)-1");
            DM_MKNOD(l, 0, "caped MKNOD");
            TEST_EXPECT_EQ((u64)g_loom_wire_gid, (u64)P9_NOGID, "caped MKNOD: gid (u32)-1");
            DM_UNLINKAT(l, 0, "caped UNLINKAT: the owner's W|X");

            g_loom_ga_valid_clear = P9_GETATTR_UID | P9_GETATTR_GID;
            TEST_EXPECT_EQ((u64)loom_ga_submit(l, 512), (u64)sizeof(struct p9_attr),
                           "caped GETATTR");
            g_loom_ga_valid_clear = 0;
            TEST_EXPECT_EQ((u64)a->uid, (u64)0x1234u, "caped GETATTR: the mounter's uid");
            TEST_EXPECT_EQ((u64)a->gid, (u64)0x5678u, "caped GETATTR: the mounter's gid");
            TEST_EXPECT_EQ((u64)(a->valid & (P9_GETATTR_UID | P9_GETATTR_GID)),
                           (u64)(P9_GETATTR_UID | P9_GETATTR_GID),
                           "caped GETATTR: the owner marked valid though the server left it out");
            TEST_EXPECT_EQ((u64)a->mode, (u64)(T_S_IFDIR | 0700u), "caped GETATTR: the server's mode");
        }

        burrow_unref(b);
        loom_unref(l);
        spoor_clunk(root);
        loom_test_ident_drop(who);
        loom_ga_reset();
        p9_client_destroy(&g_client);
        p9_loopback_destroy(&g_loopback);
    }
}

// LOOM.md 8.5.1 (audit F1): a mutation op's names get the sync twins' component
// rule -- 1..255 bytes, no '/' or NUL, not "." or ".." -- checked on the kernel
// copy the wire carries, before any stat. SYMLINK's TARGET is a path and may
// hold '/'. Every op's refusals sit beside the same op on a clean name.
static void loom_nm_leg(struct Loom *l, u8 *bkva, u8 opcode, const char *region,
                        u32 len, u64 r1, u64 r3, s32 want, const char *what) {
    for (u32 i = 0; i < len; i++) bkva[i] = (u8)region[i];
    loom_dm_leg(l, opcode, len, 0, r1, 0, r3, want, what);
}
#define NM(op, nm, len, r1, r3, want) \
    loom_nm_leg(l, bkva, LOOM_OP_##op, nm, len, r1, r3, want, #op " " #nm " -> " #want)

void test_9p_client_loom_dirmut_names(void) {
    loom_ga_reset();
    loom_ga_dir(0755u, 0x1234u, 0x5678u);
    struct Spoor *root = loom_mut_open();
    TEST_ASSERT(root != NULL, "capture client + root");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create");
    loom_install_test_handle(l, 0, root, RIGHT_READ | RIGHT_WRITE);
    loom_install_test_handle(l, 1, root, RIGHT_READ | RIGHT_WRITE);
    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    struct Proc *who = loom_test_ident(l, 0x1234u, 0x5678u, 0);
    TEST_ASSERT(who != NULL, "bind the owner");
    const s32 INVAL = -(s32)T_E_INVAL;
    const s32 ACCES = -(s32)T_E_ACCES;

    NM(MKDIR,    "a/b",  3, 0755u,    0, INVAL);
    NM(MKDIR,    ".",    1, 0755u,    0, INVAL);
    NM(MKDIR,    "..",   2, 0755u,    0, INVAL);
    NM(MKDIR,    "a\0b", 3, 0755u,    0, INVAL);
    NM(MKDIR,    "abc",  3, 0755u,    0, 0);
    NM(MKNOD,    "a/b",  3, 0010644u, 0, INVAL);
    NM(MKNOD,    "..",   2, 0010644u, 0, INVAL);
    NM(MKNOD,    "abc",  3, 0010644u, 0, 0);
    NM(UNLINKAT, "a/b",  3, 0,        0, INVAL);
    NM(UNLINKAT, ".",    1, 0,        0, INVAL);
    NM(UNLINKAT, "..",   2, 0,        0, INVAL);
    NM(UNLINKAT, "a\0b", 3, 0,        0, INVAL);
    NM(UNLINKAT, "abc",  3, 0,        0, 0);
    NM(LINK,     "a/b",  3, 0,        1, INVAL);
    NM(LINK,     "..",   2, 0,        1, INVAL);
    NM(LINK,     "abc",  3, 0,        1, 0);
    // Two-name ops split at r1: SYMLINK's name, then its target (a path).
    NM(SYMLINK,  "a/bt",     4, 3, 0, INVAL);
    NM(SYMLINK,  "..t",      3, 2, 0, INVAL);
    NM(SYMLINK,  "ok../x/y", 8, 2, 0, 0);
    NM(RENAMEAT, "..b",      3, 2, 1, INVAL);
    NM(RENAMEAT, "ax/y",     4, 1, 1, INVAL);
    NM(RENAMEAT, "a.",       2, 1, 1, INVAL);
    NM(RENAMEAT, "ab",       2, 1, 1, 0);
    // The length bound: 256 bytes is one past SYS_WALK_OPEN_NAME_MAX.
    for (u32 i = 0; i < 256; i++) bkva[i] = (u8)'x';
    loom_dm_leg(l, LOOM_OP_MKDIR, 256, 0, 0755u, 0, 0, INVAL, "MKDIR a 256-byte name -> INVAL");
    loom_dm_leg(l, LOOM_OP_MKDIR, 255, 0, 0755u, 0, 0, 0,     "MKDIR a 255-byte name -> 0");
    // The two-name spans: RENAMEAT copies both names (up to 2 x 255 bytes), SYMLINK
    // only its name -- the target is a path and stays in the buffer.
    for (u32 i = 0; i < 511; i++) bkva[i] = (u8)'x';
    loom_dm_leg(l, LOOM_OP_RENAMEAT, 510, 0, 255, 0, 1, 0,     "RENAMEAT two 255-byte names -> 0");
    loom_dm_leg(l, LOOM_OP_RENAMEAT, 511, 0, 255, 0, 1, INVAL, "RENAMEAT a 256-byte new name -> INVAL");
    loom_dm_leg(l, LOOM_OP_SYMLINK,  300, 0, 255, 0, 0, 0,     "SYMLINK a 255-byte name, 45-byte target -> 0");
    loom_dm_leg(l, LOOM_OP_SYMLINK,  300, 0, 256, 0, 0, INVAL, "SYMLINK a 256-byte name -> INVAL");
    // The name is judged before the identity: with no creator bound, a bad name
    // is still -EINVAL, and the clean one is the -EACCES the gate answers.
    l->ident = NULL;
    NM(MKDIR,    "a/b",  3, 0755u,    0, INVAL);
    NM(MKDIR,    "abc",  3, 0755u,    0, ACCES);
    l->ident = who;

    burrow_unref(b);
    loom_unref(l);
    spoor_clunk(root);
    loom_test_ident_drop(who);
    loom_ga_reset();
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}
#undef NM

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
    struct Loom *l = loom_create(8, 16, false);
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
// list, async_inflight > 1, the fan-in waiter's borrow-guard across a real
// pump, and the multi-entry loom_reap_terminal. The borrow-guard balance is
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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
    struct Loom *l = loom_create(8, 16, false);
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

// =============================================================================
// #375: a back-pressured sender's frame lives in the SHARED c->out_buf, and the
// pump/park (client_pump_or_park_locked) DROPS c->lock -- so a peer can legally
// build ITS frame into out_buf during the window, and the pre-spill retry then
// pushed built_len bytes of the PEER's frame: an equal-length peer frame (two
// msize-clamped F1 flush Twrites -- the dominant concurrent cold-build shape)
// went out as a clean DUPLICATE with the parked frame LOST, whose second reply
// landed on the freed-then-reused tag as a WRONG reply -- an Rlerror parses
// cleanly for ANY op (9P has no per-tag wire generation), so a stray
// Rlerror(ENOENT) poisoned a live Twalkgetattr into a persistent negative
// dentry (the task-#50 S3 ENOENT cluster + write-EIO); an unequal-length peer
// frame fails p9_transport_send's size==len validation -> session death.
//
// This test models the peer write DETERMINISTICALLY, single-threaded: the mq
// scribble knob overwrites out_buf during the recv that the self-pump arm makes
// INSIDE the dropped-lock window (exactly where a peer would build). Pre-spill,
// the retry sends the scribble -> the send fails -> the op dies and the session
// latches dead: this test FAILS. With the spill (the frame copied to a private
// buffer at the first EAGAIN; out_buf never re-read), the retry sends the
// intact frame: the op completes and the session stays live.
// =============================================================================
void test_9p_client_send_backpressure_spill_survives_outbuf_reuse(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname), aname, sizeof(aname), 0),
                   0, "handshake over mq transport");

    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    // Op A: async Tclunk whose Rclunk stays queued -- the reply op B's self-pump
    // will drain from inside the dropped-lock window (the same shape as the
    // #349 self_pump test).
    TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, 20, (const u8 *)"f", 1, NULL), 0,
                   "walk binds fid 20 (sync; drains clean)");
    g_async_op.loom            = l;
    g_async_op.user_data       = 0xB5B5B5B5ULL;
    g_async_op.last_result     = 0x7fffffff;
    g_async_op.completed       = false;
    g_async_op.rpc.on_complete = test_async_on_complete;
    u32 fid_a = 20;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid_a);
    TEST_EXPECT_EQ(rc, 0, "submit_async(clunk A): A in flight, its reply queued");

    // Arm ONE EAGAIN (op B's first send back-pressures) + the peer-write model:
    // the recv inside B's self-pump overwrites the head of the SHARED out_buf --
    // where B's built frame sits -- exactly as a peer's build would.
    g_mq.eagain_budget = 1;
    g_mq.scribble_buf  = g_client.out_buf;
    g_mq.scribble_len  = 64;
    g_mq.scribble_arm  = 1;

    // Op B: sync Twalk. Send -> EAGAIN -> self-pump (drops c->lock; the recv
    // fires the scribble + drains A's Rclunk) -> retry. Pre-spill the retry
    // reads the scribbled out_buf and the op/session dies; with the spill the
    // retry sends B's private copy and B completes.
    rc = p9_client_walk_one(&g_client, 0, 21, (const u8 *)"g", 1, NULL);
    TEST_EXPECT_EQ(rc, 0, "op B survives a peer rebuilding out_buf during its park (#375)");

    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the armed EAGAIN actually fired");
    TEST_EXPECT_EQ((u64)g_mq.scribble_arm, (u64)0, "the peer-write model actually fired");
    TEST_ASSERT(g_async_op.completed, "op A completed (its reply drained by B's self-pump)");
    TEST_ASSERT(!g_client.dead, "session stayed LIVE (no clobbered frame reached the wire)");

    g_mq.scribble_buf = NULL;
    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// #53 regression (revert-probing): an abandon whose Tflush hits transient c2s
// back-pressure (EAGAIN) must NOT latch the SHARED session dead -- it rolls
// the flush back to the pre-#845 ownerless reclaim, and the session stays
// fully usable: the orphan reply drains ownerlessly on the next op's reader
// and the tag pool is restored. Pre-fix, the single EAGAIN marked the whole
// session dead (every peer mount's op then failed -P9_E_IO) -- the #349
// collapse on a different send path.
void test_9p_client_abandon_async_eagain_keeps_session_alive(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname),
                                       aname, sizeof(aname), 0),
                   0, "handshake over mq transport");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    // An async op in flight: the mq transport stages its Rgetattr in the ring
    // (undrained -- nothing pumps), so inflight[tag] is still ours at abandon.
    TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, 31, (const u8 *)"f", 1, NULL),
                   0, "walk root -> fid 31");
    g_async_op.loom        = l;
    g_async_op.user_data   = 0x53535353;
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    g_async_op.rpc.on_complete = test_async_on_complete;
    u32 fid = 31;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_getattr, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async succeeds (op in flight)");
    u16 vt = g_async_op.rpc.tag;

    // The abandon's Tflush hits a transiently-full ring: ONE EAGAIN.
    g_mq.eagain_budget = 1;
    p9_client_abandon_async(&g_client, &g_async_op.rpc);
    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the flush send consumed the EAGAIN");

    // The #53 core: back-pressure is NOT a break.
    TEST_ASSERT(!g_client.dead, "session ALIVE after EAGAIN'd abandon flush");
    TEST_ASSERT(!g_async_op.completed, "abandoned op fired no completion");
    // The rollback restored the pre-#845 shape: the victim tag is still
    // ACTIVE (reserved against reuse until its late reply) and NOT
    // awaiting_flush; the never-sent flush tag was freed.
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)1,
                   "victim active; flush tag freed");
    TEST_ASSERT(g_client.session.outstanding[vt].active, "victim still reserved");
    TEST_ASSERT(!g_client.session.outstanding[vt].awaiting_flush,
                "victim not awaiting_flush (rolled back)");

    // The ownerless reclaim + live-session proof: a fresh sync op on the SAME
    // session pumps the orphan Rgetattr (clearing the victim tag) and completes.
    TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, 32, (const u8 *)"g", 1, NULL),
                   0, "a fresh op completes on the still-live session");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0,
                   "orphan reply drained ownerlessly; tag pool restored");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// A full send ring on an ASYNC submit is back-pressure, not a break -- the rule
// the sync path (client_send_flow) and the abandon Tflush already keep. The
// transport pushed zero bytes, so the op completes with the retryable
// -P9_E_AGAIN, it is taken back whole -- the tag, and the fid this Tclunk
// unbound at build, which the server still holds -- and the SHARED session
// keeps serving; resubmitted, the same op goes out. A latched-dead session here
// would fail every op of every Proc that resolves through it.
void test_9p_client_async_send_eagain_keeps_session_alive(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname),
                                       aname, sizeof(aname), 0),
                   0, "handshake over mq transport");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, 40, (const u8 *)"f", 1, NULL),
                   0, "walk root -> fid 40");
    size_t idle  = p9_session_inflight(&g_client.session);
    size_t bound = p9_session_n_bound_fids(&g_client.session);
    g_async_op.loom            = l;
    g_async_op.user_data       = 0xA6A6A6A6ULL;
    g_async_op.last_result     = 0x7fffffff;
    g_async_op.completed       = false;
    g_async_op.rpc.on_complete = test_async_on_complete;
    u32 fid = 40;

    g_mq.eagain_budget = 1;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the async send met the full ring");
    TEST_EXPECT_EQ((u64)(s64)rc, (u64)(s64)-P9_E_AGAIN,
                   "submit_async returns the retryable -P9_E_AGAIN");
    TEST_ASSERT(g_async_op.completed, "the op completed");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)-P9_E_AGAIN,
                   "its completion carries -P9_E_AGAIN, not a dead session's -EIO");
    TEST_ASSERT(!g_client.dead, "the shared session stays LIVE");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)idle,
                   "the never-sent tag is reclaimed");
    TEST_EXPECT_EQ((u64)p9_session_n_bound_fids(&g_client.session), (u64)bound,
                   "the never-sent Tclunk's fid is bound again (the server still holds it)");

    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk, &fid);
    TEST_EXPECT_EQ(rc, 0, "resubmitted, the same op goes out");
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "its reply is demuxed");
    TEST_ASSERT(g_async_op.completed, "the resubmitted op completed");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)0, "with success");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)idle,
                   "the tag pool is back to idle");

    loom_unref(l);
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// A full tag pool is a shortage, not a failure: an ASYNC submit that finds no
// free tag sends nothing and completes with the retryable -P9_E_AGAIN (it used
// to be -EIO, which a poll reported as a socket error), and once a reply frees
// a tag the same op goes out. The mq transport stages every unread reply, so
// P9_SESSION_MAX_OUTSTANDING unpumped async ops genuinely fill the pool.
static struct test_async_op g_pool_ops[P9_SESSION_MAX_OUTSTANDING];
static u32                  g_pool_fids[P9_SESSION_MAX_OUTSTANDING + 1];

static void test_async_record(struct p9_rpc *rpc, int status,
                              struct p9_dispatch_result *dr) {
    struct test_async_op *op = (struct test_async_op *)rpc;   // rpc is first
    (void)dr;
    op->last_result = (s32)status;
    op->completed   = true;
}

void test_9p_client_async_full_tag_pool_is_eagain(void) {
    int rc = p9_mq_loopback_init(&g_mq, canonical_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "mq loopback init");
    rc = p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                        p9_mq_loopback_ops_for(&g_mq), g_recv_buf, sizeof(g_recv_buf));
    TEST_EXPECT_EQ(rc, 0, "client init over mq transport");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_EXPECT_EQ(p9_client_handshake(&g_client, uname, sizeof(uname),
                                       aname, sizeof(aname), 0),
                   0, "handshake over mq transport");

    const u32 N = P9_SESSION_MAX_OUTSTANDING;
    for (u32 i = 0; i <= N; i++) {
        g_pool_fids[i] = 100u + i;
        TEST_EXPECT_EQ(p9_client_walk_one(&g_client, 0, g_pool_fids[i],
                                          (const u8 *)"f", 1, NULL),
                       0, "walk binds a fid (sync; drains its own reply)");
    }
    for (u32 i = 0; i < N; i++) {
        g_pool_ops[i].loom            = NULL;
        g_pool_ops[i].last_result     = 0x7fffffff;
        g_pool_ops[i].completed       = false;
        g_pool_ops[i].rpc.on_complete = test_async_record;
        TEST_EXPECT_EQ(p9_client_submit_async(&g_client, &g_pool_ops[i].rpc,
                                              test_build_clunk, &g_pool_fids[i]),
                       0, "an async clunk goes out; its reply stays unpumped");
    }
    TEST_ASSERT(!p9_session_has_free_tag(&g_client.session), "the tag pool is full");

    u64 sends = g_mq.sends;
    g_async_op.loom            = NULL;
    g_async_op.last_result     = 0x7fffffff;
    g_async_op.completed       = false;
    g_async_op.rpc.on_complete = test_async_record;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk,
                                &g_pool_fids[N]);
    TEST_EXPECT_EQ((u64)(s64)rc, (u64)(s64)-P9_E_AGAIN,
                   "a submit that finds no free tag returns -P9_E_AGAIN");
    TEST_ASSERT(g_async_op.completed, "the op completed");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)(s64)-P9_E_AGAIN,
                   "its completion carries -P9_E_AGAIN, not -EIO");
    TEST_EXPECT_EQ(g_mq.sends, sends, "nothing was sent");
    TEST_ASSERT(!g_client.dead, "the session stays LIVE");

    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "one staged reply is demuxed");
    TEST_ASSERT(p9_session_has_free_tag(&g_client.session), "its tag is free again");
    g_async_op.last_result = 0x7fffffff;
    g_async_op.completed   = false;
    rc = p9_client_submit_async(&g_client, &g_async_op.rpc, test_build_clunk,
                                &g_pool_fids[N]);
    TEST_EXPECT_EQ(rc, 0, "with a tag free, the same op goes out");

    for (u32 i = 0; i < N; i++)
        TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "drain a staged reply");
    u32 ok = 0;
    for (u32 i = 0; i < N; i++)
        if (g_pool_ops[i].completed && g_pool_ops[i].last_result == 0) ok++;
    TEST_EXPECT_EQ((u64)ok, (u64)N, "every pool op completed with success");
    TEST_ASSERT(g_async_op.completed, "the resubmitted op completed");
    TEST_EXPECT_EQ((u64)(s64)g_async_op.last_result, (u64)0, "with success");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0,
                   "the tag pool is empty");

    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

// =============================================================================
// FID-LIFECYCLE section 9: a Tclunk that could not be sent leaves its fid
// bound, for the closer; a Tclunk is never flushed; a flushed or abandoned
// walk's late reply binds its new fid and hands it to the orphan sink.
//
// A dying sender is a thread of a real Proc (kproc never dies), killed the way
// the group-terminate cascade kills a peer: group_exit_msg, then a wake of the
// Rendez it sleeps on, read under its wait_lock (proc_interrupt_terminate_wake's
// per-peer body) -- without the cascade's broadcast IPI, which would wake the
// idle secondaries (test_rendez_death_interrupts_sleep).
//
// The mq transport's recv never blocks, so no thread can sit in it as the
// elected reader. A test that needs a sender parked behind a reader holds the
// reader role itself (reader_active), as a peer blocked in recv would.
// =============================================================================

void test_9p_client_clunk_dying_keeps_fid_bound(void);
void test_9p_client_clunk_killed_while_parked(void);
void test_9p_client_clunk_killed_in_tag_drain(void);
void test_9p_client_clunk_dying_waiter_sends_no_flush(void);
void test_9p_client_flushed_walk_late_reply_to_sink(void);
void test_9p_client_abandoned_walk_late_reply_kept(void);
void test_9p_client_abandoned_async_clunk_not_flushed(void);
void test_9p_client_clunk_rlerror_drains_as_clunk(void);
void test_9p_client_clunk_malformed_reply_fails_closed(void);
void test_9p_client_abandoned_walk_malformed_late_reply_fails_closed(void);
void test_9p_client_flush_malformed_reply_fails_closed(void);
void test_9p_client_note_flush_honours_late_read(void);
void test_9p_client_note_flush_rflush_first_cancels(void);
void test_9p_client_note_flush_death_abandons(void);
void test_9p_client_note_flush_reader_honours_walk(void);
void test_9p_client_note_flush_full_pool_own_reply(void);
void test_9p_client_note_flush_pump_wakes_parked_flush(void);
void test_9p_client_note_flush_reader_rflush_first(void);
void test_9p_client_note_flush_reply_beats_unsent_flush(void);
void test_9p_client_note_flush_handoff_skips_staging(void);
void test_9p_client_handoff_skips_send_parked(void);
void test_9p_client_note_flush_staging_waits_for_owed_tag(void);
void test_9p_client_async_clunk_drain_waits_for_owed_tag(void);
void test_9p_client_stopped_waiter_elects_on_resume(void);
void test_9p_client_stop_parked_owner_not_owed(void);
void test_9p_client_note_flush_stop_parked_staging_not_owed(void);
void test_9p_client_handoff_skips_restopped_owner(void);
void test_9p_client_loom_enter_wakes_when_role_frees(void);

#define DY_CLUNK_ASYNC  1
#define DY_CLUNK_SYNC   2
#define DY_WALK         3
#define DY_READ         4

static struct test_dying g_dy;
static struct {
    int  op;
    u32  fid;
    int  rc;
    bool noted;       // the op runs as a call on signal(7)'s list, SIGCHLD caught
    bool setup_ok;
    u8   data[16];
} g_dyop;

static const struct viv_ksigaction g_dy_hand = { .handler = 0x4000u, .flags = 0,
                                                 .restorer = 0, .mask = 0 };

// A Linux phenotype whose SIGCHLD has a handler (proc_free frees the sigtab).
static bool dy_catch_child_exit(struct Proc *p) {
    p->sigtab = (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    if (!p->sigtab) return false;
    p->phenotype = PHENO_LINUX;
    return viv_sigtab_set(p->sigtab, VIV_SIGNOTE_CHILD_EXIT, &g_dy_hand);
}

static void dy_run(void *arg) {
    (void)arg;
    int rc = -1;
    if (g_dyop.noted) {
        // The thread sets up its own Proc before the op, so the post that
        // follows the park sees it.
        g_dyop.setup_ok = dy_catch_child_exit(current_thread()->proc);
        current_thread()->note_interruptible = true;
    }
    if (g_dyop.op == DY_CLUNK_ASYNC)
        rc = p9_client_clunk_async(&g_client, g_dyop.fid);
    else if (g_dyop.op == DY_CLUNK_SYNC)
        rc = p9_client_clunk(&g_client, g_dyop.fid);
    else if (g_dyop.op == DY_WALK)
        rc = p9_client_walk_one(&g_client, 0, g_dyop.fid, (const u8 *)"w", 1, NULL);
    else if (g_dyop.op == DY_READ) {
        u32 got = 0;
        rc = p9_client_read(&g_client, g_dyop.fid, 0, (u32)sizeof(g_dyop.data),
                            g_dyop.data, &got);
        if (rc == 0) rc = (int)got;
    }
    g_dyop.rc = rc;
}

static bool dy_launch(int op, u32 fid, bool dying, bool noted) {
    g_dyop.op       = op;
    g_dyop.fid      = fid;
    g_dyop.rc       = 0x7fffffff;
    g_dyop.noted    = noted;
    g_dyop.setup_ok = false;
    for (u32 i = 0; i < sizeof(g_dyop.data); i++) g_dyop.data[i] = 0;
    return test_dying_start(&g_dy, dy_run, NULL, dying);
}

// Run `op` on a thread of a fresh Proc; `dying` publishes its death first.
static bool dy_start(int op, u32 fid, bool dying) {
    return dy_launch(op, fid, dying, /*noted=*/false);
}

// As dy_start, for a Linux-phenotype thread in a call a caught note may
// interrupt (a SIGCHLD handler, note_interruptible).
static bool dy_start_noted(int op, u32 fid) {
    return dy_launch(op, fid, /*dying=*/false, /*noted=*/true);
}

// The server's view: every request it received, in order. With g_rec_clunk_err
// it answers a Tclunk with an Rlerror, as a server may while it clunks the fid.
// It answers the T-type g_rec_bad_reply_to with a reply no parser accepts.
#define DY_REC_MAX  256u
static u32  g_rec_n;
static u8   g_rec_type[DY_REC_MAX];
static bool g_rec_clunk_err;
static u8   g_rec_bad_reply_to;
static u8   g_rec_hold;         // a T-type the server holds unanswered (until flushed)

static int recording_responder(void *ctx, const u8 *req, size_t req_len,
                               u8 *resp, size_t resp_cap) {
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) != 0)
        return canonical_responder(ctx, req, req_len, resp, resp_cap);
    if (g_rec_n < DY_REC_MAX) g_rec_type[g_rec_n++] = type;
    if (g_rec_hold != 0 && type == g_rec_hold) return 0;   // no reply queued
    if (g_rec_bad_reply_to != 0 && type == g_rec_bad_reply_to) {
        // Two body bytes: an Rlerror carries four, an Rflush none, and an
        // Rwalk's count reads 65535.
        if (resp_cap < 9) return -1;
        u8 r = type == P9_TCLUNK ? P9_RLERROR : (u8)(type + 1);
        const u8 bad[9] = {9, 0, 0, 0, r, (u8)tag, (u8)(tag >> 8), 0xff, 0xff};
        for (u32 i = 0; i < 9; i++) resp[i] = bad[i];
        return 9;
    }
    if (type == P9_TCLUNK && g_rec_clunk_err)
        return rlerror_responder(ctx, req, req_len, resp, resp_cap);
    return canonical_responder(ctx, req, req_len, resp, resp_cap);
}

static u32 rec_count(u8 type) {
    u32 n = 0;
    for (u32 i = 0; i < g_rec_n; i++)
        if (g_rec_type[i] == type) n++;
    return n;
}

static int dy_client_open(void) {
    g_rec_n            = 0;
    g_rec_clunk_err    = false;
    g_rec_bad_reply_to = 0;
    g_rec_hold         = 0;
    if (p9_mq_loopback_init(&g_mq, recording_responder, NULL) != 0) return -1;
    if (p9_client_init(&g_client, /*root_fid=*/0, /*msize=*/8192,
                       p9_mq_loopback_ops_for(&g_mq),
                       g_recv_buf, sizeof(g_recv_buf)) != 0) return -1;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    return p9_client_handshake(&g_client, uname, sizeof(uname),
                               aname, sizeof(aname), 0);
}

static void dy_client_close(void) {
    p9_client_destroy(&g_client);
    p9_mq_loopback_destroy(&g_mq);
}

static void dy_hold_reader(bool held) {
    spin_lock(&g_client.lock);
    g_client.reader_active = held;
    spin_unlock(&g_client.lock);
}

static u32 dy_bind(u32 fid) {
    return p9_client_walk_one(&g_client, 0, fid, (const u8 *)"f", 1, NULL) == 0 ? 1u : 0u;
}

// A thread already dying when it asks to clunk is refused before the Tclunk
// is built: -P9_E_AGAIN, nothing on the wire, the fid still bound for the
// closer. Before the closer the build unbound the fid, the send was refused,
// and the server kept the fid until the session ended.
void test_9p_client_clunk_dying_keeps_fid_bound(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(30) + dy_bind(31), 2u, "walks bind 30 and 31");
    u64 sends = g_mq.sends;

    static const int ops[2]  = { DY_CLUNK_ASYNC, DY_CLUNK_SYNC };
    static const u32 fids[2] = { 30, 31 };
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT(dy_start(ops[i], fids[i], /*dying=*/true), "dying sender");
        TEST_YIELD_UNTIL(test_dying_done(&g_dy));
        test_dying_reap(&g_dy);
        TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_AGAIN,
                       "a dying clunk is refused with -P9_E_AGAIN");
        TEST_ASSERT(p9_session_fid_bound(&g_client.session, fids[i]),
                    "the fid is still bound");
        TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0,
                       "no tag held");
        TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                       "no slot held");
    }
    TEST_EXPECT_EQ(g_mq.sends, sends, "no Tclunk reached the wire");
    TEST_ASSERT(!g_client.dead, "the session stays live");

    // What the closer does: a live thread sends both.
    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 30), 0, "live async clunk");
    TEST_EXPECT_EQ(p9_client_clunk(&g_client, 31), 0, "live sync clunk");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 30) &&
                !p9_session_fid_bound(&g_client.session, 31), "both clunked");
    TEST_EXPECT_EQ((u64)rec_count(P9_TCLUNK), (u64)2, "the server saw both Tclunks");
    dy_client_close();
}

// A sender that dies while parked on back-pressure has built its Tclunk, and
// the build unbound the fid. The frame never reached the wire, so it is taken
// back whole: -P9_E_AGAIN, the tag free, the fid bound again. Sync and async.
void test_9p_client_clunk_killed_while_parked(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(40) + dy_bind(41), 2u, "walks bind 40 and 41");

    static const int ops[2]  = { DY_CLUNK_ASYNC, DY_CLUNK_SYNC };
    static const u32 fids[2] = { 40, 41 };
    for (int i = 0; i < 2; i++) {
        u64 sends = g_mq.sends;
        dy_hold_reader(true);
        g_mq.eagain_budget = 1;                  // the Tclunk meets a full c2s ring
        TEST_ASSERT(dy_start(ops[i], fids[i], /*dying=*/false), "sender");
        TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && g_client.send_waiters == 1);
        TEST_ASSERT(!p9_session_fid_bound(&g_client.session, fids[i]),
                    "the build unbound the fid");
        TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)1,
                       "the parked Tclunk holds its fid's slot");
        test_dying_kill(&g_dy);
        TEST_YIELD_UNTIL(test_dying_done(&g_dy));
        test_dying_reap(&g_dy);
        dy_hold_reader(false);
        TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_AGAIN,
                       "taken back: -P9_E_AGAIN");
        TEST_ASSERT(p9_session_fid_bound(&g_client.session, fids[i]),
                    "the fid is bound again");
        TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0,
                       "the tag is free");
        TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                       "no slot held");
        TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the send met the full ring");
        TEST_EXPECT_EQ(g_mq.sends, sends, "nothing reached the wire");
    }
    TEST_ASSERT(!g_client.dead, "the session stays live");
    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 40), 0, "live async clunk");
    TEST_EXPECT_EQ(p9_client_clunk(&g_client, 41), 0, "live sync clunk");
    TEST_EXPECT_EQ((u64)rec_count(P9_TCLUNK), (u64)2, "the server saw both Tclunks");
    dy_client_close();
}

// A clunk that finds the tag pool full drains replies until a tag frees. One
// that dies while parked there has built nothing, so its fid is still bound,
// and it is told -P9_E_AGAIN -- not -P9_E_IO, which would strand the fid.
void test_9p_client_clunk_killed_in_tag_drain(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    const u32 n = P9_SESSION_MAX_OUTSTANDING;
    u32 bound = 0;
    for (u32 i = 0; i <= n; i++) bound += dy_bind(100 + i);
    TEST_EXPECT_EQ(bound, n + 1, "walks bind 100..100+n");
    for (u32 i = 0; i < n; i++)
        TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 100 + i), 0, "fill the tag pool");
    TEST_ASSERT(!p9_session_has_free_tag(&g_client.session), "the tag pool is full");
    u64 sends = g_mq.sends;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_CLUNK_ASYNC, 100 + n, /*dying=*/false), "sender");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && g_client.send_waiters == 1);
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    dy_hold_reader(false);
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_AGAIN,
                   "a death in the tag drain is -P9_E_AGAIN");
    TEST_ASSERT(p9_session_fid_bound(&g_client.session, 100 + n), "the fid is still bound");
    TEST_EXPECT_EQ(g_mq.sends, sends, "nothing was sent");

    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 100 + n), 0,
                   "a live clunk drains a reply, then sends");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 100 + n), "clunked");
    TEST_ASSERT(!g_client.dead, "the session stays live");
    dy_client_close();
}

// A Tclunk is never flushed (flush(5)): a flush the server honours would
// cancel the clunk, and the fid -- unbound at the build -- would stay live on
// the server with nobody left to clunk it. A sync clunk whose waiter dies
// leaves its Tclunk in flight without an owner, and the Rclunk drains
// ownerless like an asynchronous clunk's.
void test_9p_client_clunk_dying_waiter_sends_no_flush(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(50), 1u, "walk binds 50");

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_CLUNK_SYNC, 50, /*dying=*/false), "sender");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && rec_count(P9_TCLUNK) == 1);
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)0, "the Tclunk is on the wire: it will happen");
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)0, "a Tclunk is never flushed");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 50), "unbound at the build");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)1,
                   "the Tclunk is in flight without an owner");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)1,
                   "it keeps its slot until the Rclunk");

    dy_hold_reader(false);
    u64 oc = g_client.demux_orphan_clunk;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the Rclunk drains");
    TEST_EXPECT_EQ(g_client.demux_orphan_clunk, oc + 1, "as an ownerless Rclunk");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0, "tag freed");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                   "slot released");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!g_client.dead, "the session stays live");
    dy_client_close();
}

static u32 g_sink_n;
static u32 g_sink_fid;

static int test_orphan_sink(void *arg, u32 fid) {
    (void)arg;
    g_sink_n++;
    g_sink_fid = fid;
    return 0;
}

// flush(5): a walk whose owner died is flushed, and a reply that arrives
// before the Rflush is honoured. The late Rwalk binds the walk's new fid,
// which nobody owns, so the client hands it to the orphan sink -- the closer,
// in production (p9_attached_root_spoor installs it).
void test_9p_client_flushed_walk_late_reply_to_sink(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    g_sink_n   = 0;
    g_sink_fid = P9_NOFID;
    p9_client_set_orphan_sink(&g_client, test_orphan_sink, NULL);

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_WALK, 60, /*dying=*/false), "walker");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 1);
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_IO, "the dead owner's walk failed");
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)1, "and was flushed");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 60), "its Rwalk is still queued");

    dy_hold_reader(false);
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the late Rwalk");
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the Rflush");
    TEST_ASSERT(p9_session_fid_bound(&g_client.session, 60),
                "the late Rwalk is honoured: fid 60 bound");
    TEST_EXPECT_EQ((u64)g_sink_n, (u64)1, "the fid went to the orphan sink");
    TEST_EXPECT_EQ((u64)g_sink_fid, (u64)60, "fid 60");
    TEST_EXPECT_EQ(g_client.orphan_handed, (u64)1, "counted as handed");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0, "tags freed");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                   "no slot held");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");

    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 60), 0, "the closer's clunk");
    dy_client_close();
}

// A walk whose owner died and whose Tflush met a full c2s ring is abandoned
// without a flush (#53). Its late reply frees the tag and binds the fid all
// the same. No sink is installed here, so the client keeps the fid: it dies
// with the session.
void test_9p_client_abandoned_walk_late_reply_kept(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_WALK, 70, /*dying=*/false), "walker");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 1);
    g_mq.eagain_budget = 1;                      // the owner's Tflush meets a full ring
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the Tflush met the full ring");
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)0, "no Tflush reached the server");
    u32 abandoned = 0;
    for (u32 t = 0; t < P9_SESSION_MAX_OUTSTANDING; t++)
        if (g_client.session.outstanding[t].active &&
            g_client.session.outstanding[t].abandoned) abandoned++;
    TEST_EXPECT_EQ((u64)abandoned, (u64)1, "the walk is abandoned");

    dy_hold_reader(false);
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the late Rwalk");
    TEST_ASSERT(p9_session_fid_bound(&g_client.session, 70), "the late Rwalk bound fid 70");
    TEST_EXPECT_EQ(g_client.orphan_kept, (u64)1, "with no sink the fid is kept");
    TEST_EXPECT_EQ(g_client.orphan_handed, (u64)0, "and not handed");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0, "tag freed");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                   "no slot held");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");

    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 70), 0, "a live clunk");
    dy_client_close();
}

// A Tclunk is never flushed, however its owner goes. An asynchronous one that
// is abandoned stays in flight without an owner, like p9_client_clunk_async's,
// and its Rclunk drains ownerless. A flush the server honoured would cancel
// the clunk, and the fid -- unbound at the build -- would stay on the server.
static bool          g_dy_fired;
static struct p9_rpc g_dy_rpc;

static void dy_on_complete(struct p9_rpc *rpc, int status,
                           struct p9_dispatch_result *dr) {
    (void)rpc; (void)status; (void)dr;
    g_dy_fired = true;
}

void test_9p_client_abandoned_async_clunk_not_flushed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(80), 1u, "walk binds 80");
    g_dy_fired = false;
    for (size_t i = 0; i < sizeof(g_dy_rpc); i++) ((u8 *)&g_dy_rpc)[i] = 0;
    g_dy_rpc.on_complete = dy_on_complete;
    u32 fid = 80;
    TEST_EXPECT_EQ(p9_client_submit_async(&g_client, &g_dy_rpc, test_build_clunk, &fid), 0,
                   "an async Tclunk in flight");
    p9_client_abandon_async(&g_client, &g_dy_rpc);
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)0, "an abandoned Tclunk is not flushed");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)1,
                   "the Tclunk alone is in flight");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 80), "unbound at the build");

    u64 oc = g_client.demux_orphan_clunk;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the Rclunk drains");
    TEST_EXPECT_EQ(g_client.demux_orphan_clunk, oc + 1, "as an ownerless Rclunk");
    TEST_ASSERT(!g_dy_fired, "the abandoned op completes nothing");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0, "tag freed");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                   "slot released");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!g_client.dead, "the session stays live");
    dy_client_close();
}

// A server may answer a Tclunk with an Rlerror; the fid is clunked all the
// same. For a Tclunk without an owner that reply is the clunk's, not a stray:
// it frees the tag and the slot, and it is not reported as an ownerless frame.
void test_9p_client_clunk_rlerror_drains_as_clunk(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(90), 1u, "walk binds 90");
    g_rec_clunk_err = true;
    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 90), 0, "an async Tclunk");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)1,
                   "it keeps its slot until the reply");

    u64 oc = g_client.demux_orphan_clunk;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the Rlerror drains");
    TEST_EXPECT_EQ(g_client.demux_orphan_clunk, oc + 1, "as the clunk's reply");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "not as an unexplained frame");
    TEST_EXPECT_EQ((u64)p9_session_inflight(&g_client.session), (u64)0, "tag freed");
    TEST_EXPECT_EQ((u64)p9_session_n_reserved_slots(&g_client.session), (u64)0,
                   "slot released");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 90), "the fid is gone");
    TEST_ASSERT(!g_client.dead, "the session stays live");
    g_rec_clunk_err = false;
    dy_client_close();
}

// A reply that fails to parse, to a Tclunk nobody owns, leaves its tag and slot
// held. The session fails closed, as it does for an owned op's; before, the
// ownerless arm dropped the failure and the slot leaked without a word.
void test_9p_client_clunk_malformed_reply_fails_closed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(100), 1u, "walk binds 100");
    g_rec_bad_reply_to = P9_TCLUNK;
    TEST_EXPECT_EQ(p9_client_clunk_async(&g_client, 100), 0, "an async Tclunk");

    u64 oc = g_client.demux_orphan_clunk;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the malformed Rlerror");
    TEST_EXPECT_EQ(g_client.demux_orphan_clunk, oc + 1, "reached the clunk's arm");
    TEST_ASSERT(g_client.dead, "the session failed closed");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");
    g_rec_bad_reply_to = 0;
    dy_client_close();
}

// So can the late reply of a walk abandoned without a flush: it is dispatched
// against the walk's tag, and a failure leaves that tag held.
void test_9p_client_abandoned_walk_malformed_late_reply_fails_closed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    g_rec_bad_reply_to = P9_TWALK;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_WALK, 110, /*dying=*/false), "walker");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 1);
    g_mq.eagain_budget = 1;                      // the owner's Tflush meets a full ring
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    TEST_EXPECT_EQ((u64)g_mq.eagain_budget, (u64)0, "the Tflush met the full ring");
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)0, "the walk is abandoned unflushed");

    dy_hold_reader(false);
    u64 ol = g_client.demux_orphan_late;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the malformed late Rwalk");
    TEST_EXPECT_EQ(g_client.demux_orphan_late, ol + 1, "reached the late arm");
    TEST_ASSERT(g_client.dead, "the session failed closed");
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 110), "it bound nothing");
    TEST_EXPECT_EQ(g_client.orphan_kept + g_client.orphan_handed, (u64)0, "no orphan fid");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");
    g_rec_bad_reply_to = 0;
    dy_client_close();
}

// And so can an Rflush: one that fails to parse frees neither the flush's tag
// nor the flushed walk's. The late Rwalk before it is well formed and absorbed.
void test_9p_client_flush_malformed_reply_fails_closed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    g_rec_bad_reply_to = P9_TFLUSH;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start(DY_WALK, 120, /*dying=*/false), "walker");
    TEST_YIELD_UNTIL(test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 1);
    test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL(test_dying_done(&g_dy));
    test_dying_reap(&g_dy);
    TEST_EXPECT_EQ((u64)rec_count(P9_TFLUSH), (u64)1, "the walk was flushed");

    dy_hold_reader(false);
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the late Rwalk");
    TEST_ASSERT(!g_client.dead, "a well-formed late reply is absorbed");
    u64 of = g_client.demux_orphan_flush;
    TEST_EXPECT_EQ(p9_client_reader_pump_ready(&g_client), 1, "the malformed Rflush");
    TEST_EXPECT_EQ(g_client.demux_orphan_flush, of + 1, "reached the flush's arm");
    TEST_ASSERT(g_client.dead, "the session failed closed");
    TEST_EXPECT_EQ(g_client.demux_orphan, (u64)0, "no unexplained frame");
    g_rec_bad_reply_to = 0;
    dy_client_close();
}

// =============================================================================
// flush(5) on a caught-note unwind (ARCH 8.8.3, 21.10). A thread that LIVES
// sends its Tflush and waits, killable only, for the first answer: a reply
// that beats the Rflush is honoured and the call completes with it; an Rflush
// that comes first cancels the op (-P9_E_INTR); a death in that wait drops the
// registration and leaves both answers to drain ownerless, as #845 does.
//
// The op runs on a Linux-phenotype thread in a call on signal(7)'s list, and
// the note is a child_exit its SIGCHLD handler catches, posted through the
// exit path's own code (the rendez.caught_wake_* shape). The test holds the
// reader role, so the op parks as a non-reader and only the note's wake ends
// its first wait. Each leg finishes and reaps its thread before it asserts
// anything, so a RED run leaks no parked thread.
// =============================================================================

// A child's exit posts child_exit to `par`, through the exit path's own code.
static void dy_post_child_exit(struct Proc *par) {
    struct Proc *kid = proc_alloc();
    if (!kid) return;
    kid->parent = par;   // the notify reads only parent, pid and exit_status
    irq_state_t s = proc_table_lock_acquire();
    proc_exit_notify_parent_locked(kid);
    proc_table_lock_release(s);
    kid->parent = NULL;
    kid->state  = PROC_STATE_ZOMBIE;
    proc_free(kid);
}

// Park the noted op behind the held reader, post the caught note, and report
// whether the op then sent a Tflush and went on waiting (instead of returning).
static bool dy_note_and_flush(u8 op_type, bool *parked) {
    TEST_YIELD_UNTIL_SOFT(test_dying_parked(&g_dy) && rec_count(op_type) == 1);
    *parked = test_dying_parked(&g_dy) && !test_dying_done(&g_dy);
    if (!*parked) return false;
    dy_post_child_exit(g_dy.proc);
    TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_dy) ||
                          (rec_count(P9_TFLUSH) == 1 && test_dying_parked(&g_dy)));
    return rec_count(P9_TFLUSH) == 1 && !test_dying_done(&g_dy);
}

// End an op thread whatever state a leg left it in, then reap it. A thread
// still waiting after the budget is killed, and *killed says so.
static void dy_finish_of(struct test_dying *d, bool *killed) {
    TEST_YIELD_UNTIL_SOFT(test_dying_done(d));
    *killed = !test_dying_done(d);
    if (*killed) {
        test_dying_kill(d);
        TEST_YIELD_UNTIL_SOFT(test_dying_done(d));
    }
    test_dying_reap(d);
}

static void dy_finish(bool *killed) { dy_finish_of(&g_dy, killed); }

// The server had already answered the read, so its Rread comes before the
// Rflush: the read returns its bytes, not EINTR, and its tag stays reserved
// until the Rflush frees it.
void test_9p_client_note_flush_honours_late_read(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(130), 1u, "walk binds 130");

    dy_hold_reader(true);
    TEST_ASSERT(dy_start_noted(DY_READ, 130), "reader");
    bool parked;
    bool waited = dy_note_and_flush(P9_TREAD, &parked);
    dy_hold_reader(false);
    int  pr = p9_client_reader_pump_ready(&g_client);          // the Rread
    bool killed;
    dy_finish(&killed);
    u64  mid      = p9_session_inflight(&g_client.session);
    u64  late     = g_client.demux_orphan_late;
    int  pf       = p9_client_reader_pump_ready(&g_client);    // the Rflush
    u64  end      = p9_session_inflight(&g_client.session);
    u64  honoured = g_client.flush_honoured;
    u64  cancel   = g_client.flush_cancelled;
    u64  orphan   = g_client.demux_orphan;
    bool dead     = g_client.dead;
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(waited, "the interrupted read waits for its Tflush's answer");
    TEST_EXPECT_EQ(pr, 1, "the Rread, before the Rflush");
    TEST_ASSERT(!killed, "the read returned on its own");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5,
                   "flush(5): a reply that beats the Rflush is honoured -- the bytes, not EINTR");
    TEST_ASSERT(g_dyop.data[0] == 'h' && g_dyop.data[4] == 'o', "the bytes the server sent");
    TEST_EXPECT_EQ(honoured, (u64)1, "counted as honoured");
    TEST_EXPECT_EQ(late, (u64)0, "not absorbed as an ownerless late reply");
    TEST_EXPECT_EQ(mid, (u64)2, "its tag stays reserved until the Rflush (I-10)");
    TEST_EXPECT_EQ(pf, 1, "the Rflush");
    TEST_EXPECT_EQ(end, (u64)0, "the Rflush frees both tags");
    TEST_EXPECT_EQ(cancel, (u64)0, "nothing cancelled");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the session stays live");
}

// The server holds the read and answers the flush: the Rflush comes first, the
// op was cancelled, and only then does the read return -P9_E_INTR. Until then a
// reply could still act on the read's fid, so the fid is not clunked under it.
void test_9p_client_note_flush_rflush_first_cancels(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(131), 1u, "walk binds 131");
    g_rec_hold = P9_TREAD;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start_noted(DY_READ, 131), "reader");
    bool parked;
    bool waited  = dy_note_and_flush(P9_TREAD, &parked);
    u64  waiting = p9_session_inflight(&g_client.session);
    int  ck_wait = waited ? p9_client_clunk_async(&g_client, 131) : 0;
    bool kept    = p9_session_fid_bound(&g_client.session, 131);
    dy_hold_reader(false);
    int  pf = p9_client_reader_pump_ready(&g_client);          // the Rflush
    bool killed;
    dy_finish(&killed);
    u64  end      = p9_session_inflight(&g_client.session);
    int  ck_done  = p9_client_clunk_async(&g_client, 131);
    int  pc       = ck_done == 0 ? p9_client_reader_pump_ready(&g_client) : 0;  // the Rclunk
    u64  after    = p9_session_inflight(&g_client.session);
    u64  honoured = g_client.flush_honoured;
    u64  cancel   = g_client.flush_cancelled;
    u64  orphan   = g_client.demux_orphan;
    bool dead     = g_client.dead;
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(waited, "the interrupted read waits for its Tflush's answer");
    TEST_EXPECT_EQ(waiting, (u64)2, "the read's tag and the flush's are in flight");
    TEST_EXPECT_EQ(ck_wait, -P9_E_IO, "while its owner waits, the read's fid is not clunked");
    TEST_ASSERT(kept, "and stays bound");
    TEST_EXPECT_EQ(pf, 1, "the Rflush");
    TEST_ASSERT(!killed, "the Rflush ended the wait");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_INTR,
                   "an Rflush that comes first cancels the read: -EINTR");
    TEST_EXPECT_EQ(cancel, (u64)1, "counted as cancelled");
    TEST_EXPECT_EQ(honoured, (u64)0, "nothing honoured");
    TEST_EXPECT_EQ(end, (u64)0, "the Rflush freed both tags");
    TEST_EXPECT_EQ(ck_done, 0, "cancelled, the read no longer holds its fid: the clunk goes out");
    TEST_EXPECT_EQ(pc, 1, "the Rclunk");
    TEST_EXPECT_EQ(after, (u64)0, "the Rclunk freed its tag");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the session stays live");
}

// A death in the flush wait abandons the op as #845 does: the Tflush is already
// on the wire, so nothing more is sent, and its Rflush drains ownerless.
void test_9p_client_note_flush_death_abandons(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(132), 1u, "walk binds 132");
    g_rec_hold = P9_TREAD;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start_noted(DY_READ, 132), "reader");
    bool parked;
    bool waited = dy_note_and_flush(P9_TREAD, &parked);
    if (waited) test_dying_kill(&g_dy);
    TEST_YIELD_UNTIL_SOFT(test_dying_done(&g_dy));
    bool died   = test_dying_done(&g_dy);
    u32  sends  = rec_count(P9_TFLUSH);
    // With its owner gone the op's fid is the closer's, before the Rflush lands.
    int  ck     = died ? p9_client_clunk_async(&g_client, 132) : -1;
    dy_hold_reader(false);
    u64  of     = g_client.demux_orphan_flush;
    int  pf     = p9_client_reader_pump_ready(&g_client);      // the Rflush
    int  pc     = p9_client_reader_pump_ready(&g_client);      // the Rclunk
    bool killed;
    dy_finish(&killed);
    u64  flushed = g_client.demux_orphan_flush - of;
    u64  end     = p9_session_inflight(&g_client.session);
    u64  cancel  = g_client.flush_cancelled;
    u64  orphan  = g_client.demux_orphan;
    bool dead    = g_client.dead;
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(waited, "the interrupted read waits for its Tflush's answer");
    TEST_ASSERT(died, "the kill ends the flush wait");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_IO, "a death is -EIO, as #845");
    TEST_EXPECT_EQ((u64)sends, (u64)1, "no second Tflush");
    TEST_EXPECT_EQ(ck, 0, "its owner gone, the op's fid is clunked before the Rflush");
    TEST_EXPECT_EQ(pf, 1, "the Rflush");
    TEST_EXPECT_EQ(pc, 1, "the Rclunk");
    TEST_EXPECT_EQ(flushed, (u64)1, "drained ownerless");
    TEST_EXPECT_EQ(cancel, (u64)0, "no waiter left to cancel");
    TEST_EXPECT_EQ(end, (u64)0, "the Rflush freed both tags");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the session stays live");
}

// The flush wait can hold the reader role: handed it, the interrupted walk
// reads its own Rwalk, honours it -- the walk's caller gets its fid, not the
// closer -- and leaves the Rflush to the next reader.
void test_9p_client_note_flush_reader_honours_walk(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    g_sink_n   = 0;
    g_sink_fid = P9_NOFID;
    p9_client_set_orphan_sink(&g_client, test_orphan_sink, NULL);

    dy_hold_reader(true);
    TEST_ASSERT(dy_start_noted(DY_WALK, 133), "walker");
    bool parked;
    bool waited = dy_note_and_flush(P9_TWALK, &parked);
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);                      // to the walk itself
    bool killed;
    dy_finish(&killed);
    bool bound    = p9_session_fid_bound(&g_client.session, 133);
    u64  mid      = p9_session_inflight(&g_client.session);
    u64  honoured = g_client.flush_honoured;
    int  pf       = p9_client_reader_pump_ready(&g_client);    // the Rflush
    u64  end      = p9_session_inflight(&g_client.session);
    u32  sink_n   = g_sink_n;
    u64  orphan   = g_client.demux_orphan;
    bool dead     = g_client.dead;
    int  clunk    = p9_client_clunk_async(&g_client, 133);
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the walk parked behind the held reader");
    TEST_ASSERT(waited, "the interrupted walk waits for its Tflush's answer");
    TEST_ASSERT(!killed, "the walk returned on its own");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)0, "flush(5): the walk completes");
    TEST_ASSERT(bound, "its fid is bound");
    TEST_EXPECT_EQ((u64)sink_n, (u64)0, "the walk's caller holds the fid, not the closer");
    TEST_EXPECT_EQ(honoured, (u64)1, "counted as honoured");
    TEST_EXPECT_EQ(mid, (u64)2, "its tag stays reserved until the Rflush");
    TEST_EXPECT_EQ(pf, 1, "the Rflush, left for the next reader");
    TEST_EXPECT_EQ(end, (u64)0, "both tags freed");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(clunk, 0, "the walk's fid clunks");
}

// Fill the tag pool with `n` never-sent fsyncs on the root fid, the way a busy
// shared session holds its tags; dy_unfill_pool takes them back.
static u32 dy_fill_pool(u32 n, u16 *tags) {
    static u8 frame[64];
    u32 got = 0;
    spin_lock(&g_client.lock);
    for (u32 i = 0; i < n; i++) {
        int len = p9_session_send_fsync(&g_client.session, frame, sizeof(frame), 0, 0);
        if (len <= 0) break;
        u32 sz; u8 ty; u16 t;
        if (p9_peek_header(frame, (size_t)len, &sz, &ty, &t) < 0) break;
        tags[got++] = t;
    }
    spin_unlock(&g_client.lock);
    return got;
}

static void dy_unfill_pool(u32 n, const u16 *tags) {
    spin_lock(&g_client.lock);
    for (u32 i = 0; i < n; i++) p9_session_abort_unsent(&g_client.session, tags[i]);
    spin_unlock(&g_client.lock);
}

// A full tag pool and no reader: the interrupted read cannot stage its Tflush,
// so it pumps for a tag -- and the frame it pumps is its own Rread. The reply
// beat the flush outright: the read returns its bytes, no Tflush goes out, and
// the read stops pumping at its answer (on the mq loopback one more read finds
// the queue empty, an EOF that kills the session).
void test_9p_client_note_flush_full_pool_own_reply(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(134), 1u, "walk binds 134");
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 1, tags);

    dy_hold_reader(true);
    bool started = dy_start_noted(DY_READ, 134);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TREAD) == 1));
    bool parked = started && test_dying_parked(&g_dy) && !test_dying_done(&g_dy);
    bool full   = !p9_session_has_free_tag(&g_client.session);
    dy_hold_reader(false);                     // nobody reads: the read pumps itself
    if (parked) dy_post_child_exit(g_dy.proc);
    bool killed = false;
    if (started) dy_finish(&killed);
    u32  flushes = rec_count(P9_TFLUSH);
    bool dead    = g_client.dead;
    dy_unfill_pool(filled, tags);
    u64  end     = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 1), "63 tags held");
    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(full, "with the read's own tag the pool is full");
    TEST_ASSERT(!killed, "the read returned on its own");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5,
                   "its own reply, pumped while it waited for a tag, completes it");
    TEST_EXPECT_EQ((u64)flushes, (u64)0, "no Tflush was owed: the reply came first");
    TEST_ASSERT(!dead, "the read stopped pumping at its answer");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The flush wait holds the reader role and the Rflush comes first: handed the
// role, the interrupted read reads the Rflush for its own op, which cancels it.
// The read stops reading there (on the mq loopback one more read finds the
// queue empty, an EOF that kills the session) and returns -P9_E_INTR.
void test_9p_client_note_flush_reader_rflush_first(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(136), 1u, "walk binds 136");
    g_rec_hold = P9_TREAD;

    dy_hold_reader(true);
    TEST_ASSERT(dy_start_noted(DY_READ, 136), "reader");
    bool parked;
    bool waited = dy_note_and_flush(P9_TREAD, &parked);
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);                      // to the read itself
    bool killed;
    dy_finish(&killed);
    u64  end      = p9_session_inflight(&g_client.session);
    u64  honoured = g_client.flush_honoured;
    u64  cancel   = g_client.flush_cancelled;
    u64  orphan   = g_client.demux_orphan;
    bool dead     = g_client.dead;
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(waited, "the interrupted read waits for its Tflush's answer");
    TEST_ASSERT(!killed, "the Rflush it read ended the wait");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_INTR,
                   "the Rflush the read demuxed itself cancels it: -EINTR");
    TEST_EXPECT_EQ(cancel, (u64)1, "counted as cancelled");
    TEST_EXPECT_EQ(honoured, (u64)0, "nothing honoured");
    TEST_EXPECT_EQ(end, (u64)0, "the Rflush freed both tags");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the read stopped reading at its Rflush");
}

// The Tflush meets a full send ring and parks for progress. The pump that wakes
// it demuxes the read's own Rread, which lands before the Tflush is on the wire.
// With the answer in hand no flush is owed: the Tflush goes back unsent, and the
// read completes with its bytes as an ordinary reply, both tags freed at once.
void test_9p_client_note_flush_reply_beats_unsent_flush(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(137), 1u, "walk binds 137");

    dy_hold_reader(true);
    bool started = dy_start_noted(DY_READ, 137);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TREAD) == 1));
    bool parked = started && test_dying_parked(&g_dy) && !test_dying_done(&g_dy);
    g_mq.eagain_budget = 1;                     // the Tflush's first send meets a full ring
    if (parked) dy_post_child_exit(g_dy.proc);
    TEST_YIELD_UNTIL_SOFT(!parked || test_dying_done(&g_dy) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dy)));
    bool on_list = parked && g_client.send_waiters == 1 && !test_dying_done(&g_dy);
    u32  unfired = g_mq.eagain_budget;
    dy_hold_reader(false);
    int  pr = on_list ? p9_client_reader_pump_ready(&g_client) : 0;   // the Rread
    bool killed = false;
    if (started) dy_finish(&killed);
    g_mq.eagain_budget = 0;
    u64  end      = p9_session_inflight(&g_client.session);
    u32  flushes  = rec_count(P9_TFLUSH);
    u64  honoured = g_client.flush_honoured;
    u64  late     = g_client.demux_orphan_late;
    u64  cancel   = g_client.flush_cancelled;
    u64  orphan   = g_client.demux_orphan;
    bool dead     = g_client.dead;
    dy_client_close();

    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(on_list, "interrupted, its Tflush parks on the full ring");
    TEST_EXPECT_EQ((u64)unfired, (u64)0, "the armed EAGAIN fired");
    TEST_EXPECT_EQ(pr, 1, "the pump demuxes the read's own Rread");
    TEST_ASSERT(!killed, "the pump's departure woke the flush");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5,
                   "flush(5): the reply that came before the Tflush left completes the read");
    TEST_ASSERT(g_dyop.data[0] == 'h' && g_dyop.data[4] == 'o', "the bytes the server sent");
    TEST_EXPECT_EQ((u64)flushes, (u64)0, "the Tflush went back unsent");
    TEST_EXPECT_EQ(honoured, (u64)0, "an ordinary reply: nothing awaited an Rflush");
    TEST_EXPECT_EQ(end, (u64)0, "both tags freed at once");
    TEST_EXPECT_EQ(late, (u64)0, "not absorbed as an ownerless late reply");
    TEST_EXPECT_EQ(cancel, (u64)0, "nothing cancelled");
    TEST_EXPECT_EQ(orphan, (u64)0, "no unexplained frame");
    TEST_ASSERT(!dead, "the session stays live");
}

// A full tag pool and a busy reader: the interrupted read parks for progress.
// The reader is a pump (p9_client_reader_pump_ready, as the SQPOLL and
// dev9p-poll kthreads run it) and the frame it demuxes is the read's own Rread,
// which wakes only the read's rendez -- not the send list the read sleeps on.
// The pump must signal progress when it departs, or the read sleeps on with its
// answer in hand.
void test_9p_client_note_flush_pump_wakes_parked_flush(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(135), 1u, "walk binds 135");
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 1, tags);

    dy_hold_reader(true);
    bool started = dy_start_noted(DY_READ, 135);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TREAD) == 1));
    bool parked = started && test_dying_parked(&g_dy) && !test_dying_done(&g_dy);
    if (parked) dy_post_child_exit(g_dy.proc);
    TEST_YIELD_UNTIL_SOFT(!parked || test_dying_done(&g_dy) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dy)));
    bool on_list = parked && g_client.send_waiters == 1 && !test_dying_done(&g_dy);
    dy_hold_reader(false);
    int  pr = on_list ? p9_client_reader_pump_ready(&g_client) : 0;   // the Rread
    bool killed = false;
    if (started) dy_finish(&killed);
    u32  flushes = rec_count(P9_TFLUSH);
    bool dead    = g_client.dead;
    dy_unfill_pool(filled, tags);
    u64  end     = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 1), "63 tags held");
    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "the read parked behind the held reader");
    TEST_ASSERT(on_list, "interrupted, it parks for a tag on the send list");
    TEST_EXPECT_EQ(pr, 1, "the pump demuxes the read's own Rread");
    TEST_ASSERT(!killed, "the pump's departure woke the read");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5, "and its reply completes it");
    TEST_EXPECT_EQ((u64)flushes, (u64)0, "no Tflush was owed");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// A second op beside the one under test: a plain read on its own thread.
static struct test_dying g_dyx;
static struct { u32 fid; int rc; } g_dyxop;

static void dyx_run(void *arg) {
    (void)arg;
    u8  buf[16];
    u32 got = 0;
    int rc = p9_client_read(&g_client, g_dyxop.fid, 0, (u32)sizeof(buf), buf, &got);
    g_dyxop.rc = rc == 0 ? (int)got : rc;
}

static bool dyx_start(u32 fid) {
    g_dyxop.fid = fid;
    g_dyxop.rc  = 0x7fffffff;
    return test_dying_start(&g_dyx, dyx_run, NULL, /*dead_now=*/false);
}

// Under the client lock: the lowest tag above `after` with a registered op,
// and the tag of the op designated to read next; -1 for none.
static int dy_registered_tag_above(int after) {
    int t = -1;
    spin_lock(&g_client.lock);
    for (int i = after + 1; i < (int)P9_SESSION_MAX_OUTSTANDING && t < 0; i++)
        if (g_client.inflight[i]) t = i;
    spin_unlock(&g_client.lock);
    return t;
}

static int dy_designated_tag(void) {
    int t = -1;
    spin_lock(&g_client.lock);
    for (int i = 0; i < (int)P9_SESSION_MAX_OUTSTANDING && t < 0; i++)
        if (g_client.inflight[i] && g_client.inflight[i]->be_reader) t = i;
    spin_unlock(&g_client.lock);
    return t;
}

// The reader role never goes to an op still getting its frame onto the wire:
// that thread sleeps on the send list, where a designation cannot reach it,
// and a stop or a death there would take the role with it. The interrupted
// walk S (held by the server) stages its Tflush on a full pool and parks on the
// send list; beside it the read X waits in client_wait with its reply queued.
// S holds the lower tag, so a handoff that ignored `sending` would pick it and
// strand X. Designated, X reads its reply; the tag X frees lets S send its
// Tflush, and S then reads its own Rflush.
void test_9p_client_note_flush_handoff_skips_staging(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(139), 1u, "walk binds 139");
    g_rec_hold = P9_TWALK;
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 2, tags);

    dy_hold_reader(true);
    bool started = dy_start_noted(DY_WALK, 138);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 2));
    int  s_tag     = dy_registered_tag_above(-1);
    bool x_started = started && dyx_start(139);
    TEST_YIELD_UNTIL_SOFT(!x_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    int  x_tag = dy_registered_tag_above(s_tag);
    bool full  = !p9_session_has_free_tag(&g_client.session);
    bool both  = x_started && test_dying_parked(&g_dy) && test_dying_parked(&g_dyx) &&
                 !test_dying_done(&g_dy) && !test_dying_done(&g_dyx);
    if (both) dy_post_child_exit(g_dy.proc);
    TEST_YIELD_UNTIL_SOFT(!both || test_dying_done(&g_dy) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dy)));
    bool staging = both && g_client.send_waiters == 1 && !test_dying_done(&g_dy);
    dy_hold_reader(false);
    int des = -1;
    if (staging) {
        p9_client_handoff_reader(&g_client);                  // as a departing reader does
        des = dy_designated_tag();
    }
    bool x_killed = false, s_killed = false;
    if (x_started) dy_finish_of(&g_dyx, &x_killed);
    if (started) dy_finish(&s_killed);
    bool bound = p9_session_fid_bound(&g_client.session, 138);
    bool dead  = g_client.dead;
    dy_unfill_pool(filled, tags);
    u64  end   = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 2), "62 tags held");
    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(both, "both ops parked behind the held reader");
    TEST_ASSERT(s_tag >= 0 && x_tag > s_tag, "the interrupted walk holds the lower tag");
    TEST_ASSERT(full, "the pool is full");
    TEST_ASSERT(staging, "interrupted, the walk parks on the send list to stage its Tflush");
    // X may take the role and consume its flag before it is read; the op still
    // sending, never woken by a designation, would keep it.
    TEST_ASSERT(des != s_tag, "the handoff does not designate the op still sending");
    TEST_ASSERT(!x_killed, "the read took the role and read its own reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "the read completes with the server's bytes");
    TEST_ASSERT(!s_killed, "the walk sent its Tflush once a tag freed");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_INTR,
                   "and read its own Rflush first: -EINTR");
    TEST_ASSERT(!bound, "the cancelled walk bound nothing");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The same for the #349 send park: the read S meets a full send ring and parks
// on the send list with its frame unsent, while the read X waits in client_wait
// with its reply queued. Designated, X reads its reply and leaves; S then sends
// and reads its own.
void test_9p_client_handoff_skips_send_parked(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(140) + dy_bind(141), 2u, "walks bind 140 and 141");

    dy_hold_reader(true);
    g_mq.eagain_budget = 1;                         // S's Tread meets a full ring
    bool started = dy_start(DY_READ, 140, /*dying=*/false);
    TEST_YIELD_UNTIL_SOFT(!started || test_dying_done(&g_dy) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dy)));
    bool s_parked  = started && g_client.send_waiters == 1 && !test_dying_done(&g_dy);
    u32  unfired   = g_mq.eagain_budget;
    int  s_tag     = dy_registered_tag_above(-1);
    bool x_started = s_parked && dyx_start(141);
    TEST_YIELD_UNTIL_SOFT(!x_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool x_parked = x_started && test_dying_parked(&g_dyx) && !test_dying_done(&g_dyx);
    int  x_tag    = dy_registered_tag_above(s_tag);
    dy_hold_reader(false);
    int des = -1;
    if (x_parked) {
        p9_client_handoff_reader(&g_client);                  // as a departing reader does
        des = dy_designated_tag();
    }
    bool x_killed = false, s_killed = false;
    if (x_started) dy_finish_of(&g_dyx, &x_killed);
    if (started) dy_finish(&s_killed);
    g_mq.eagain_budget = 0;
    bool dead = g_client.dead;
    u64  end  = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_ASSERT(s_parked, "S parks on the full send ring");
    TEST_EXPECT_EQ((u64)unfired, (u64)0, "the armed EAGAIN fired");
    TEST_ASSERT(x_parked, "X parked behind the held reader");
    TEST_ASSERT(s_tag >= 0 && x_tag > s_tag, "the unsent read holds the lower tag");
    TEST_ASSERT(des != s_tag, "the handoff does not designate the sender");
    TEST_ASSERT(!x_killed, "X took the role and read its own reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "X completes with the server's bytes");
    TEST_ASSERT(!s_killed, "S sent once X left, and read its own reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5, "S completes with the server's bytes");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// A tag an owner is about to free needs no frame. The interrupted walk A finds
// the pool full and no reader, so it pumps one frame -- the read Y's Rread --
// and Y's tag is then owed: Y's dispatch frees it, and no frame announces that.
// A waits for the dispatch instead of reading again (on the mq loopback one more
// read finds the queue empty, an EOF that kills the session), sends its Tflush
// on the tag Y freed, and reads its own Rflush first.
void test_9p_client_note_flush_staging_waits_for_owed_tag(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(142), 1u, "walk binds 142");
    g_rec_hold = P9_TWALK;
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 2, tags);

    dy_hold_reader(true);
    bool y_started = dyx_start(142);
    TEST_YIELD_UNTIL_SOFT(!y_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool started = y_started && dy_start_noted(DY_WALK, 143);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TWALK) == 2));
    bool full = !p9_session_has_free_tag(&g_client.session);
    bool both = started && test_dying_parked(&g_dy) && test_dying_parked(&g_dyx) &&
                !test_dying_done(&g_dy) && !test_dying_done(&g_dyx);
    dy_hold_reader(false);                     // no reader, and nobody designated
    if (both) dy_post_child_exit(g_dy.proc);
    bool a_killed = false, y_killed = false;
    if (started) dy_finish(&a_killed);
    if (y_started) dy_finish_of(&g_dyx, &y_killed);
    u32  flushes = rec_count(P9_TFLUSH);
    bool bound   = p9_session_fid_bound(&g_client.session, 143);
    bool dead    = g_client.dead;
    dy_unfill_pool(filled, tags);
    u64  end     = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 2), "62 tags held");
    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(both, "both ops parked behind the held reader");
    TEST_ASSERT(full, "the pool is full");
    TEST_ASSERT(!a_killed, "Y's dispatch woke the walk");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)(s64)-P9_E_INTR,
                   "the walk sent its Tflush on the tag Y freed and read its Rflush first");
    TEST_EXPECT_EQ((u64)flushes, (u64)1, "one Tflush went out");
    TEST_ASSERT(!y_killed, "Y completed");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "Y completes with the server's bytes");
    TEST_ASSERT(!bound, "the cancelled walk bound nothing");
    TEST_ASSERT(!dead, "no read past Y's reply: the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The same for the async clunk's tag drain (FID-LIFECYCLE section 9): it finds
// the pool full and no reader, pumps the read Y's Rread, then waits for Y's
// dispatch to free a tag instead of reading again.
void test_9p_client_async_clunk_drain_waits_for_owed_tag(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(144) + dy_bind(145), 2u, "walks bind 144 and 145");
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 1, tags);

    dy_hold_reader(true);
    bool y_started = dyx_start(144);
    TEST_YIELD_UNTIL_SOFT(!y_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool y_parked = y_started && test_dying_parked(&g_dyx) && !test_dying_done(&g_dyx);
    bool full     = !p9_session_has_free_tag(&g_client.session);
    dy_hold_reader(false);                     // no reader, and nobody designated
    bool started = y_parked && dy_start(DY_CLUNK_ASYNC, 145, /*dying=*/false);
    bool c_killed = false, y_killed = false;
    if (started) dy_finish(&c_killed);
    if (y_started) dy_finish_of(&g_dyx, &y_killed);
    u32  clunks = rec_count(P9_TCLUNK);
    bool bound  = p9_session_fid_bound(&g_client.session, 145);
    bool dead   = g_client.dead;
    int  pc     = dead ? 0 : p9_client_reader_pump_ready(&g_client);   // the Rclunk
    dy_unfill_pool(filled, tags);
    u64  end    = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 1), "63 tags held");
    TEST_ASSERT(y_parked, "Y parked behind the held reader");
    TEST_ASSERT(full, "the pool is full");
    TEST_ASSERT(started, "the clunk ran");
    TEST_ASSERT(!c_killed, "Y's dispatch woke the clunk");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)0, "the Tclunk went out on the tag Y freed");
    TEST_EXPECT_EQ((u64)clunks, (u64)1, "one Tclunk");
    TEST_ASSERT(!bound, "its fid unbound");
    TEST_ASSERT(!y_killed, "Y completed");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "Y completes with the server's bytes");
    TEST_ASSERT(!dead, "no read past Y's reply: the session stays live");
    TEST_EXPECT_EQ(pc, 1, "the Rclunk drains ownerless");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// =============================================================================
// 9P waiters and stops (DEBUG-FS-DESIGN 5c.6, the 2026-09-30 amendment). A
// thread waiting in the client for someone else's reading is stopped the way a
// debugger or ^Z stops it. Every reply must still get a reader, and neither the
// reader handoff nor a tag drainer may count on a thread parked for a stop.
// =============================================================================

// Stop or resume a test thread's Proc through the kernel's own entry points:
// the debugger's axis, or the job axis (^Z, /proc suspend).
static void dy_stop(struct test_dying *d, bool job) {
    irq_state_t s = proc_table_lock_acquire();
    if (job) (void)proc_job_stop_proc(d->proc);
    else     proc_debug_stop_deliver(d->proc);
    proc_table_lock_release(s);
}

static void dy_resume(struct test_dying *d, bool job) {
    irq_state_t s = proc_table_lock_acquire();
    if (job) proc_job_cont_proc(d->proc);
    else     proc_debug_resume(d->proc);
    proc_table_lock_release(s);
}

// Parked for a stop: asleep on its own debug_rendez.
static bool dy_stop_parked(const struct test_dying *d) {
    return test_dying_parked(d) &&
           __atomic_load_n(&d->t->rendez_blocked_on, __ATOMIC_ACQUIRE) == &d->t->debug_rendez;
}

// A stop that clears and comes back before its thread runs leaves only the
// flag moving: set it alone, with no wake, and the parked thread stays parked.
static void dy_stop_flag(struct test_dying *d, bool job, u32 v) {
    __atomic_store_n(job ? &d->proc->job_stop_req : &d->proc->debug_stop_req, v,
                     __ATOMIC_RELEASE);
}

// A third op thread: an async clunk.
static struct test_dying g_dyz;
static struct { u32 fid; int rc; } g_dyzop;

static void dyz_run(void *arg) {
    (void)arg;
    g_dyzop.rc = p9_client_clunk_async(&g_client, g_dyzop.fid);
}

static bool dyz_start_clunk(u32 fid) {
    g_dyzop.fid = fid;
    g_dyzop.rc  = 0x7fffffff;
    return test_dying_start(&g_dyz, dyz_run, NULL, /*dead_now=*/false);
}

// The read X waits behind the held reader with its Rread queued and is stopped
// there (^Z). The reader departs with nobody to designate, and X resumes. X
// must then take the role and read its own reply. Parked in place, it re-checked
// only its own sleep condition and slept on with its answer unread.
void test_9p_client_stopped_waiter_elects_on_resume(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(146), 1u, "walk binds 146");

    dy_hold_reader(true);
    bool started = dyx_start(146);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool waiting = started && test_dying_parked(&g_dyx) && !test_dying_done(&g_dyx);
    if (waiting) dy_stop(&g_dyx, /*job=*/true);
    TEST_YIELD_UNTIL_SOFT(!waiting || dy_stop_parked(&g_dyx));
    bool stopped = waiting && dy_stop_parked(&g_dyx);
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);                  // as a departing reader does
    int des = dy_designated_tag();
    if (started) dy_resume(&g_dyx, /*job=*/true);
    bool killed = false;
    if (started) dy_finish_of(&g_dyx, &killed);
    bool dead = g_client.dead;
    u64  end  = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_ASSERT(waiting, "the read waits behind the held reader");
    TEST_ASSERT(stopped, "stopped (^Z), it parks");
    TEST_EXPECT_EQ((u64)(s64)des, (u64)(s64)-1, "nobody is designated to read");
    TEST_ASSERT(!killed, "resumed, it takes the role and reads its own reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "the read completes with the server's bytes");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The read X is stopped behind the held reader and resumed while that reader
// still holds the role, so X sleeps on its rpc again as a plain waiter. When
// the reader departs, the handoff must designate X. Had the resume left X
// marked stop-parked, the handoff would skip it and its reply would sit unread.
void test_9p_client_resumed_waiter_is_designated(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(155), 1u, "walk binds 155");

    dy_hold_reader(true);
    bool started = dyx_start(155);
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool waiting = started && test_dying_parked(&g_dyx) && !test_dying_done(&g_dyx);
    if (waiting) dy_stop(&g_dyx, /*job=*/true);
    TEST_YIELD_UNTIL_SOFT(!waiting || dy_stop_parked(&g_dyx));
    bool stopped = waiting && dy_stop_parked(&g_dyx);
    if (stopped) dy_resume(&g_dyx, /*job=*/true);
    TEST_YIELD_UNTIL_SOFT(!stopped || test_dying_done(&g_dyx) ||
                          (test_dying_parked(&g_dyx) && !dy_stop_parked(&g_dyx)));
    bool resleeps = stopped && test_dying_parked(&g_dyx) && !dy_stop_parked(&g_dyx) &&
                    !test_dying_done(&g_dyx);
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);                  // as a departing reader does
    bool killed = false;
    if (started) dy_finish_of(&g_dyx, &killed);
    bool dead = g_client.dead;
    u64  end  = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_ASSERT(waiting, "the read waits behind the held reader");
    TEST_ASSERT(stopped, "stopped (^Z), it parks");
    TEST_ASSERT(resleeps, "resumed while the reader still reads, it sleeps on its rpc again");
    // Nothing else wakes X: nobody reads its reply, and the session stays up.
    TEST_ASSERT(!killed, "designated, it takes the role and reads its own reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "the read completes with the server's bytes");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// A tag drainer must not wait on the dispatch of an owner parked for a stop.
// The read Y waits behind the held reader and is stopped; its Rread is then read,
// so Y's tag waits for Y's dispatch. The clunk C takes the last tag, its Rclunk
// queued. Y's stop clears and comes back before Y runs, and in between the
// async clunk D finds the pool full with no reader. Reading the flag while it
// was clear, D counted Y's tag as owed and waited out the second stop; it must
// read on instead, and C's Rclunk frees a tag.
void test_9p_client_stop_parked_owner_not_owed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(147) + dy_bind(148) + dy_bind(149), 3u, "walks bind 147..149");
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 2, tags);

    dy_hold_reader(true);
    bool y_started = dyx_start(147);
    TEST_YIELD_UNTIL_SOFT(!y_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 1));
    bool y_waiting = y_started && test_dying_parked(&g_dyx) && !test_dying_done(&g_dyx);
    if (y_waiting) dy_stop(&g_dyx, /*job=*/false);
    TEST_YIELD_UNTIL_SOFT(!y_waiting || dy_stop_parked(&g_dyx));
    bool y_stopped = y_waiting && dy_stop_parked(&g_dyx);
    dy_hold_reader(false);
    int  py   = y_stopped ? p9_client_reader_pump_ready(&g_client) : 0;   // Y's Rread
    int  cc   = y_stopped ? p9_client_clunk_async(&g_client, 148) : -1;  // C
    bool full = !p9_session_has_free_tag(&g_client.session);
    if (y_started) dy_stop_flag(&g_dyx, /*job=*/false, 0u);              // the stop clears
    bool started = y_stopped && full && dyz_start_clunk(149);            // D
    TEST_YIELD_UNTIL_SOFT(!started || test_dying_done(&g_dyz) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dyz)));
    if (y_started) dy_stop_flag(&g_dyx, /*job=*/false, 1u);              // and comes back
    bool d_killed = false, y_killed = false;
    if (started) dy_finish_of(&g_dyz, &d_killed);
    if (y_started) dy_resume(&g_dyx, /*job=*/false);
    if (y_started) dy_finish_of(&g_dyx, &y_killed);
    u32  clunks = rec_count(P9_TCLUNK);
    bool dead   = g_client.dead;
    int  pd     = (!dead && started && !d_killed) ? p9_client_reader_pump_ready(&g_client) : 0;
    dy_unfill_pool(filled, tags);
    u64  end    = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 2), "62 tags held");
    TEST_ASSERT(y_waiting, "Y waits behind the held reader");
    TEST_ASSERT(y_stopped, "stopped, Y parks");
    TEST_EXPECT_EQ(py, 1, "Y's Rread is read while Y is stopped");
    TEST_EXPECT_EQ(cc, 0, "C takes the last tag");
    TEST_ASSERT(full, "the pool is full");
    TEST_ASSERT(started, "D ran");
    TEST_ASSERT(!d_killed, "D read on past the stopped owner instead of waiting for it");
    TEST_EXPECT_EQ((u64)(s64)g_dyzop.rc, (u64)0, "D's Tclunk went out on the tag C freed");
    TEST_EXPECT_EQ((u64)clunks, (u64)2, "two Tclunks");
    TEST_ASSERT(!y_killed, "Y completed once resumed");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "Y completes with the server's bytes");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(pd, 1, "D's Rclunk drains ownerless");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The same for an owner stopped while it stages a flush(5) Tflush: the
// interrupted read S parks on the send list for a tag (the pool is full, the
// reader held), is stopped there (^Z), and its own Rread is read. Parked inside
// the send list's sleep, S was invisible to the client, and a drainer that read
// S's stop flag while it was clear counted on S's dispatch.
void test_9p_client_note_flush_stop_parked_staging_not_owed(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(150) + dy_bind(151) + dy_bind(152), 3u, "walks bind 150..152");
    u16 tags[P9_SESSION_MAX_OUTSTANDING];
    u32 filled = dy_fill_pool(P9_SESSION_MAX_OUTSTANDING - 2, tags);

    dy_hold_reader(true);
    bool started = dy_start_noted(DY_READ, 150);                         // S
    TEST_YIELD_UNTIL_SOFT(!started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TREAD) == 1));
    bool parked = started && test_dying_parked(&g_dy) && !test_dying_done(&g_dy);
    int  cc     = parked ? p9_client_clunk_async(&g_client, 151) : -1;   // C
    bool full   = !p9_session_has_free_tag(&g_client.session);
    if (parked && full) dy_post_child_exit(g_dy.proc);
    TEST_YIELD_UNTIL_SOFT(!parked || !full || test_dying_done(&g_dy) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dy)));
    bool staging = parked && full && g_client.send_waiters == 1 && !test_dying_done(&g_dy);
    if (staging) dy_stop(&g_dy, /*job=*/true);
    TEST_YIELD_UNTIL_SOFT(!staging || dy_stop_parked(&g_dy));
    bool s_stopped = staging && dy_stop_parked(&g_dy);
    dy_hold_reader(false);
    int  ps = s_stopped ? p9_client_reader_pump_ready(&g_client) : 0;     // S's Rread
    if (started) dy_stop_flag(&g_dy, /*job=*/true, 0u);                  // the stop clears
    bool d_started = s_stopped && dyz_start_clunk(152);                  // D
    TEST_YIELD_UNTIL_SOFT(!d_started || test_dying_done(&g_dyz) ||
                          (g_client.send_waiters == 1 && test_dying_parked(&g_dyz)));
    if (started) dy_stop_flag(&g_dy, /*job=*/true, 1u);                  // and comes back
    bool d_killed = false, s_killed = false;
    if (d_started) dy_finish_of(&g_dyz, &d_killed);
    if (started) dy_resume(&g_dy, /*job=*/true);
    if (started) dy_finish(&s_killed);
    u32  flushes = rec_count(P9_TFLUSH);
    bool dead    = g_client.dead;
    int  pd      = (!dead && d_started && !d_killed) ? p9_client_reader_pump_ready(&g_client) : 0;
    dy_unfill_pool(filled, tags);
    u64  end     = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_EXPECT_EQ((u64)filled, (u64)(P9_SESSION_MAX_OUTSTANDING - 2), "62 tags held");
    TEST_ASSERT(g_dyop.setup_ok, "a Linux phenotype whose SIGCHLD is caught");
    TEST_ASSERT(parked, "S waits behind the held reader");
    TEST_EXPECT_EQ(cc, 0, "C takes the last tag");
    TEST_ASSERT(full, "the pool is full");
    TEST_ASSERT(staging, "interrupted, S parks on the send list to stage its Tflush");
    TEST_ASSERT(s_stopped, "stopped (^Z) there, S parks");
    TEST_EXPECT_EQ(ps, 1, "S's Rread is read while S is stopped");
    TEST_ASSERT(d_started, "D ran");
    TEST_ASSERT(!d_killed, "D read on past the stopped owner instead of waiting for it");
    TEST_EXPECT_EQ((u64)(s64)g_dyzop.rc, (u64)0, "D's Tclunk went out on the tag C freed");
    TEST_ASSERT(!s_killed, "S completed once resumed");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5, "its reply beat the flush: the read's bytes");
    TEST_EXPECT_EQ((u64)flushes, (u64)0, "no Tflush went out");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(pd, 1, "D's Rclunk drains ownerless");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// The reads Y (lower tag) and X wait behind the held reader, their replies
// queued. Y is stopped, and its stop clears and comes back without Y running. A
// departing reader that read Y's flag in between designated Y -- a wake that
// landed on nothing, since Y sleeps on its debug_rendez -- and X, never
// designated, slept on with nobody reading.
void test_9p_client_handoff_skips_restopped_owner(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    TEST_EXPECT_EQ(dy_bind(153) + dy_bind(154), 2u, "walks bind 153 and 154");

    dy_hold_reader(true);
    bool y_started = dy_start(DY_READ, 153, /*dying=*/false);            // Y
    TEST_YIELD_UNTIL_SOFT(!y_started ||
                          (test_dying_parked(&g_dy) && rec_count(P9_TREAD) == 1));
    int  y_tag     = dy_registered_tag_above(-1);
    bool x_started = y_started && dyx_start(154);                        // X
    TEST_YIELD_UNTIL_SOFT(!x_started ||
                          (test_dying_parked(&g_dyx) && rec_count(P9_TREAD) == 2));
    int  x_tag = dy_registered_tag_above(y_tag);
    bool both  = x_started && test_dying_parked(&g_dy) && test_dying_parked(&g_dyx) &&
                 !test_dying_done(&g_dy) && !test_dying_done(&g_dyx);
    if (both) dy_stop(&g_dy, /*job=*/false);
    TEST_YIELD_UNTIL_SOFT(!both || dy_stop_parked(&g_dy));
    bool y_stopped = both && dy_stop_parked(&g_dy);
    if (y_started) dy_stop_flag(&g_dy, /*job=*/false, 0u);               // the stop clears
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);                  // as a departing reader does
    int des = dy_designated_tag();
    if (y_started) dy_stop_flag(&g_dy, /*job=*/false, 1u);               // and comes back
    bool x_killed = false, y_killed = false;
    if (x_started) dy_finish_of(&g_dyx, &x_killed);
    if (y_started) dy_resume(&g_dy, /*job=*/false);
    if (y_started) dy_finish(&y_killed);
    bool dead = g_client.dead;
    u64  end  = p9_session_inflight(&g_client.session);
    dy_client_close();

    TEST_ASSERT(both, "both reads wait behind the held reader");
    TEST_ASSERT(y_tag >= 0 && x_tag > y_tag, "Y holds the lower tag");
    TEST_ASSERT(y_stopped, "stopped, Y parks");
    // X may take the role and consume its flag before it is read.
    TEST_ASSERT(des != y_tag, "the parked owner is not designated");
    TEST_ASSERT(!x_killed, "X took the role and read its reply");
    TEST_EXPECT_EQ((u64)(s64)g_dyxop.rc, (u64)5, "X completes with the server's bytes");
    TEST_ASSERT(!y_killed, "Y completed once resumed");
    TEST_EXPECT_EQ((u64)(s64)g_dyop.rc, (u64)5, "Y completes with the server's bytes");
    TEST_ASSERT(!dead, "the session stays live");
    TEST_EXPECT_EQ(end, (u64)0, "every tag freed");
}

// A Loom ENTER whose pump finds the reader role held sleeps for a CQE. On a
// shared client that reader may be another Proc's sync op, which reads only
// until its own reply and hands the role only to sync ops. The test holds the
// role while the ENTER submits a READ and sleeps, then leaves the way such a
// reader does, with the READ's Rread still queued: the ENTER must wake and read
// it. Before, it slept until unrelated sync traffic happened to read it.
static struct test_dying g_dle;
static struct { struct Loom *l; int n; } g_dleop;

static void dle_run(void *arg) {
    (void)arg;
    g_dleop.n = loom_enter(g_dleop.l, 1, 1, 0);
}

void test_9p_client_loom_enter_wakes_when_role_frees(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    TEST_ASSERT(sp != NULL, "dev9p_attach_client");
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    rights_t rt = RIGHT_READ | RIGHT_WRITE;
    TEST_ASSERT(loom_register_handles(l, &sp, &rt, 1) == 0, "register the dev9p spoor");
    struct Burrow *b; u8 *bkva;
    loom_install_test_buf(l, 0, PAGE_SIZE, &b, &bkva);
    for (u32 i = 0; i < 8; i++) bkva[i] = 0xAA;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    cl_stage_rw(l, 0, LOOM_OP_READ, /*handle=*/0, /*offset=*/0, /*count=*/5,
                /*bidx=*/0, /*buf_off=*/0, 0xFEED000000000005ULL);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    dy_hold_reader(true);                         // a reader that is not the ENTER's
    g_dleop.l = l;
    g_dleop.n = -2;
    bool started = test_dying_start(&g_dle, dle_run, NULL, /*dead_now=*/false);
    TEST_YIELD_UNTIL_SOFT(!started || test_dying_done(&g_dle) ||
                          (test_dying_parked(&g_dle) && rec_count(P9_TREAD) == 1));
    bool asleep = started && test_dying_parked(&g_dle) && !test_dying_done(&g_dle);
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);          // it leaves; no sync op to designate
    bool killed = false;
    if (started) dy_finish_of(&g_dle, &killed);
    u32  cq   = l->cq_tail;
    u64  ud   = cqes[0].user_data;
    s64  res  = (s64)cqes[0].result;
    bool read = bkva[0] == 'h' && bkva[4] == 'o';
    bool dead = g_client.dead;
    burrow_unref(b);
    loom_unref(l);
    dy_client_close();

    TEST_ASSERT(asleep, "the ENTER sleeps behind the held reader");
    TEST_ASSERT(!killed, "the reader's departure woke it");
    TEST_EXPECT_EQ((u64)(s64)g_dleop.n, (u64)1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)cq, (u64)1, "one CQE posted");
    TEST_EXPECT_EQ(ud, 0xFEED000000000005ULL, "user_data echoed");
    TEST_EXPECT_EQ((u64)res, (u64)5, "READ result = 5 bytes read");
    TEST_ASSERT(read, "the Rread payload reached the registered buffer");
    TEST_ASSERT(!dead, "the session stays live");
}

// Two clients behind one ring (LOOM.md 8.6, the 2026-10-06 amendment). The
// ring's newest op rides a client whose reader role another thread holds and
// never reads with; an older op rides a second client whose reply is already
// queued. The ENTER reads for every client it waits on, so it reads the second
// client's reply and returns while the first stays held. Before, it pumped only
// the newest op's client, slept on that client's role, and the second client's
// reply stayed unread for as long as the role was held.
static struct p9_client      g_client2;
static struct p9_mq_loopback g_mq2;
static u8                    g_recv_buf2[8192];
static struct test_dying     g_dle2;
static struct { struct Loom *l; int n; } g_dle2op;

static void dle2_run(void *arg) {
    (void)arg;
    g_dle2op.n = loom_enter(g_dle2op.l, 2, 1, 0);
}

static int cl2_open(void) {
    if (p9_mq_loopback_init(&g_mq2, canonical_responder, NULL) != 0) return -1;
    if (p9_client_init(&g_client2, /*root_fid=*/0, /*msize=*/8192,
                       p9_mq_loopback_ops_for(&g_mq2),
                       g_recv_buf2, sizeof(g_recv_buf2)) != 0) return -1;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    return p9_client_handshake(&g_client2, uname, sizeof(uname),
                               aname, sizeof(aname), 0);
}

static void cl2_close(void) {
    p9_client_destroy(&g_client2);
    p9_mq_loopback_destroy(&g_mq2);
}

// One leg: client 1 (g_client) held, its op newest; client 2's reply queued.
// `cap` is the fan-in test cap (0 = the full set). Returns through *out.
struct two_client_out {
    bool started, returned, killed, dead1, dead2;
    int  n;
    u32  cq_first;
    u64  ud_first;
    s64  res_first;
    u32  cq_end;
};
static void two_client_leg(u32 cap, struct two_client_out *o) {
    *o = (struct two_client_out){0};
    struct Spoor *sp1 = dev9p_attach_client(&g_client, 0);
    struct Spoor *sp2 = dev9p_attach_client(&g_client2, 0);
    struct Loom *l = (sp1 && sp2) ? loom_create(8, 16, false) : NULL;
    struct Spoor *sps[2] = { sp2, sp1 };
    rights_t rts[2] = { RIGHT_READ | RIGHT_WRITE, RIGHT_READ | RIGHT_WRITE };
    if (!l || loom_register_handles(l, sps, rts, 2) != 0) {
        if (l) loom_unref(l);
        return;
    }
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    // SQE 0 -> client 2 (the older op); SQE 1 -> client 1 (the newest, the
    // in-flight list's head).
    cl_stage_sqe(l, 0, LOOM_OP_FSYNC, /*handle=*/0, /*datasync*/0, 0xB0B0000000000002ULL);
    cl_stage_sqe(l, 1, LOOM_OP_FSYNC, /*handle=*/1, /*datasync*/0, 0xA0A0000000000001ULL);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    __atomic_store_n(&g_loom_fanin_test_cap, cap, __ATOMIC_RELEASE);
    dy_hold_reader(true);                         // client 1: held, never read
    g_dle2op.l = l;
    g_dle2op.n = -2;
    o->started = test_dying_start(&g_dle2, dle2_run, NULL, /*dead_now=*/false);
    TEST_YIELD_UNTIL_SOFT(!o->started || test_dying_done(&g_dle2));
    o->returned  = o->started && test_dying_done(&g_dle2);
    o->cq_first  = l->cq_tail;
    o->ud_first  = cqes[0].user_data;
    o->res_first = (s64)cqes[0].result;
    // Let client 1 go and drain its reply, then end the thread either way.
    dy_hold_reader(false);
    p9_client_handoff_reader(&g_client);
    if (o->started) dy_finish_of(&g_dle2, &o->killed);
    __atomic_store_n(&g_loom_fanin_test_cap, 0u, __ATOMIC_RELEASE);
    o->n = g_dle2op.n;
    (void)p9_client_reader_pump_ready(&g_client);   // client 1's Rfsync, if unread
    o->cq_end = l->cq_tail;
    o->dead1  = g_client.dead;
    o->dead2  = g_client2.dead;
    loom_unref(l);
}

void test_9p_client_loom_enter_reads_every_client(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client 1 over mq");
    TEST_EXPECT_EQ(cl2_open(), 0, "client 2 over mq");
    struct two_client_out o;
    two_client_leg(0, &o);
    cl2_close();
    dy_client_close();

    TEST_ASSERT(o.started, "the ENTER thread started");
    TEST_ASSERT(o.returned, "the ENTER returned while client 1's role stayed held");
    TEST_ASSERT(!o.killed, "nothing had to kill it");
    TEST_EXPECT_EQ((u64)(s64)o.n, (u64)2, "both SQEs consumed");
    TEST_EXPECT_EQ((u64)o.cq_first, (u64)1, "one CQE when it returned");
    TEST_EXPECT_EQ(o.ud_first, 0xB0B0000000000002ULL, "client 2's op completed first");
    TEST_EXPECT_EQ((u64)o.res_first, (u64)0, "fsync success");
    TEST_EXPECT_EQ((u64)o.cq_end, (u64)2, "client 1's op completed once released");
    TEST_ASSERT(!o.dead1 && !o.dead2, "both sessions stay live");
}

// More clients in flight than the fan-in set holds (a re-register with ops in
// flight): the set is partial, so the waiter rescans on a timer from a rotating
// cursor. The cap of 1 leaves client 2 out of the first set -- its reply has no
// hook to wake the ENTER -- and the rescan must still find it.
void test_9p_client_loom_enter_partial_set_rescans(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client 1 over mq");
    TEST_EXPECT_EQ(cl2_open(), 0, "client 2 over mq");
    struct two_client_out o;
    two_client_leg(1, &o);
    cl2_close();
    dy_client_close();

    TEST_ASSERT(o.started, "the ENTER thread started");
    TEST_ASSERT(o.returned, "the rescan found client 2's reply");
    TEST_ASSERT(!o.killed, "nothing had to kill it");
    TEST_EXPECT_EQ(o.ud_first, 0xB0B0000000000002ULL, "client 2's op completed first");
    TEST_EXPECT_EQ((u64)o.cq_end, (u64)2, "client 1's op completed once released");
    TEST_ASSERT(!o.dead1 && !o.dead2, "both sessions stay live");
}

// An SQPOLL ring's op rides a client whose reader role another thread holds and
// never reads with. The kthread may not read that reply, so it hooks the role
// and parks: asleep, never run, for as long as the role stays held. The role's
// release wakes it to read the reply. Before, the pump reported the role busy
// and the kthread yielded and pumped again, a CPU spent on a reply it could
// not read. Everything is observed first and judged after the teardown, so a
// failure leaves no kthread hooked on the client the runner destroys.
void test_9p_client_loom_sqpoll_parks_on_a_held_role(void) {
    TEST_EXPECT_EQ(dy_client_open(), 0, "client over mq");
    struct Spoor *sp = dev9p_attach_client(&g_client, 0);
    struct Proc  *p  = sp ? proc_alloc() : NULL;
    struct loom_params kp;
    hidx_t fd = -1;
    struct Handle hh;
    bool got = p && sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd) == 0 &&
               handle_get(p, fd, &hh) == 0;
    struct Loom *l = got ? (struct Loom *)hh.obj : NULL;
    bool ring_up = l && l->sqpoll;
    bool parked = false, stayed = false, posted = false;
    u64  runs = ~0ull;
    s64  res  = -1;
    if (ring_up) {
        loom_install_test_handle(l, 0, sp, RIGHT_READ | RIGHT_WRITE);
        struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
        struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
        struct Thread *kt = l->sqpoll;
        dy_hold_reader(true);
        cl_stage_sqe(l, 0, LOOM_OP_FSYNC, /*handle=*/0, /*datasync*/0, 0xC0C0000000000003ULL);
        __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
        (void)sys_loom_enter_for_proc(p, fd, 0, 0, 0);    // wakes the kthread
        // The Tfsync is out and its reply queued: from here the kthread has
        // nothing it may read.
        TEST_YIELD_UNTIL_SOFT(rec_count(P9_TFSYNC) == 1u && g_mq.tail != g_mq.head &&
                              __atomic_load_n(&kt->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING);
        parked = rec_count(P9_TFSYNC) == 1u && g_mq.tail != g_mq.head &&
                 __atomic_load_n(&kt->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING;
        if (parked) {
            u64 n0 = __atomic_load_n(&kt->nsched, __ATOMIC_ACQUIRE);
            u64 t0 = timer_now_ns();
            TEST_YIELD_UNTIL_SOFT(timer_now_ns() >= t0 + 50ull * 1000ull * 1000ull);
            runs   = __atomic_load_n(&kt->nsched, __ATOMIC_ACQUIRE) - n0;
            stayed = __atomic_load_n(&kt->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING &&
                     __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) == 0u;
        }
        dy_hold_reader(false);
        p9_client_handoff_reader(&g_client);              // the role comes free
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) >= 1u);
        posted = __atomic_load_n(&h->cq_tail, __ATOMIC_ACQUIRE) >= 1u;
        res    = (s64)cqes[0].result;
    }
    if (got) handle_put(&hh);
    if (p) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);                     // the last handle: loom_free joins the kthread
    }
    if (sp) spoor_clunk(sp);
    dy_client_close();

    TEST_ASSERT(ring_up, "SQPOLL ring with its kthread");
    TEST_ASSERT(parked, "the kthread parked, the op's client role held");
    TEST_ASSERT(stayed, "and stayed parked, the reply unread");
    TEST_EXPECT_EQ(runs, (u64)0, "never ran while the role stayed held");
    TEST_ASSERT(posted, "the role's release woke it to read the reply");
    TEST_EXPECT_EQ((u64)res, (u64)0, "fsync success");
}

// The runner's release, after every test (test.c). A test that fails before its
// last lines leaves its op threads asleep in g_client, and the client and its
// transports open. The next test's open re-inits the transport under such a
// thread, which can then wake into the new test's client as a stale reader or
// waiter. So every op thread still up is killed and reaped BEFORE the client
// goes: a destroy under a sleeper frees what it sleeps on. A Loom ring a test
// leaves alive is not seen here; the ring tests release theirs before their
// verdicts. Returns whether anything was left up.
bool test_9p_client_release(void);
bool test_9p_client_release(void) {
    __atomic_store_n(&g_loom_fanin_test_cap, 0u, __ATOMIC_RELEASE);
    struct test_dying *ops[] = { &g_dy, &g_dyx, &g_dyz, &g_dle, &g_dle2 };
    bool left = false;
    for (u32 i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        struct test_dying *d = ops[i];
        if (!d->t) continue;
        left = true;
        if (!test_dying_done(d)) test_dying_kill(d);
        test_dying_reap(d);
        if (d->t) return true;          // it never exited: leave what it sleeps in
    }
    if (g_client.magic == P9_CLIENT_MAGIC) {
        left = true;
        p9_client_destroy(&g_client);
    }
    if (g_loopback.magic == P9_LOOPBACK_MAGIC) {
        left = true;
        p9_loopback_destroy(&g_loopback);
    }
    if (g_mq.magic == P9_MQ_LOOPBACK_MAGIC) {
        left = true;
        p9_mq_loopback_destroy(&g_mq);
    }
    if (g_client2.magic == P9_CLIENT_MAGIC) {
        left = true;
        p9_client_destroy(&g_client2);
    }
    if (g_mq2.magic == P9_MQ_LOOPBACK_MAGIC) {
        left = true;
        p9_mq_loopback_destroy(&g_mq2);
    }
    return left;
}
