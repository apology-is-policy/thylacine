// dev9p Dev vtable unit tests (P5-attach-dev).
//
// Composes loopback transport + p9_client + dev9p root Spoor; exercises
// the Dev vtable surface (walk / open / read / write / close) end-to-end
// and verifies the operations route through the 9P client to the
// loopback responder. The lower layers (codec / session / transport /
// client) each have their own test suites; these tests verify the proxy
// composition.

#include "test.h"

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_transport_loopback.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/poll.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/caps.h>
#include <thylacine/handle.h>

void test_dev9p_registered(void);
void test_dev9p_attach_client_root_spoor(void);
void test_dev9p_walk_one_component(void);
void test_dev9p_walk_clone(void);
void test_dev9p_open_lopens_fid(void);
void test_dev9p_read_routes_through_client(void);
void test_dev9p_write_routes_through_client(void);
void test_dev9p_close_clunks_owned_fid(void);
void test_dev9p_close_does_not_clunk_root_fid(void);
void test_dev9p_create_file(void);
void test_dev9p_create_dir(void);
void test_dev9p_fsync(void);
void test_dev9p_readdir(void);
void test_dev9p_poll_regular_file_always_ready(void);
void test_dev9p_prw_wire_offset_and_cursor(void);
void test_dev9p_wstat_readonly_fd(void);
void test_dev9p_walk_attrs(void);

// File-scope buffers — client is ~12 KiB, won't fit on the 16 KiB test
// thread stack alongside a few smaller locals.
static struct p9_client g_client;
static struct p9_loopback g_loopback;
static u8 g_recv_buf[4096];
static u8 g_loopback_resp[4096];

// A-2a: capture the Tsetattr fields dev9p_wstat_native put on the wire so a
// test can assert the T_WSTAT_* -> P9_SETATTR_* mask + values reached it.
static u32 g_setattr_valid, g_setattr_mode, g_setattr_uid, g_setattr_gid;
static u32 le32_at(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64_at(const u8 *p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8 * i);
    return v;
}
// #955: capture the Treaddir request's `offset` (the opaque resume cookie) so a
// test can assert dev9p_readdir forwards a high-bit (>INT64_MAX) cookie verbatim
// rather than sign-clamping it to 0.
static u64 g_readdir_req_offset;

// #37: capture the Tread/Twrite request offsets so the positioned-I/O tests
// can assert the CALLER's offset (SYS_PREAD/SYS_PWRITE) vs the Spoor cursor
// (SYS_READ/SYS_WRITE) is what actually reaches the wire.
static u64 g_tread_req_offset, g_twrite_req_offset;

// POUNCE: when set, the Twalkgetattr responder answers one component SHORT
// (a partial walk) -- the session layer must then leave newfid unbound and
// dev9p_walk_attrs must leave nc untouched.
static bool g_dev9p_wga_partial;

// Canonical responder — synthesizes valid Rmsgs for every op type
// dev9p might issue. Mirrors the responder used in test_9p_client.c
// but adds Rclunk handling for the close path.
static int dev9p_responder(void *ctx, const u8 *req, size_t req_len,
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
        // Inspect Twalk body to learn nwname; respond with that many
        // qids so partial-walk checks pass. Body offset: hdr(7) +
        // fid(4) + newfid(4) = 15; nwname at 15..17.
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
            resp[off + 5] = (u8)(0x10 + i);     // distinguishable path
            for (int j = 1; j < 8; j++) resp[off + 5 + j] = 0;
        }
        return (int)total;
    }
    if (type == P9_TCLUNK) {
        // Empty body.
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
        resp[12] = 0x42; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        resp[20] = 0; resp[21] = 0x10; resp[22] = 0; resp[23] = 0;     // iounit=4096
        return (int)total;
    }
    if (type == P9_TREAD) {
        // Capture the request offset (Tread body: fid@7, offset@11, count@19).
        if (req_len >= P9_HDR_LEN + 4 + 8 + 4)
            g_tread_req_offset = le64_at(req + 11);
        // Respond with 5-byte payload "hello".
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
        // Echo accepted count = requested count.
        if (req_len < P9_HDR_LEN + 4 + 8 + 4) return -1;
        // Capture the request offset (Twrite body: fid@7, offset@11, count@19).
        g_twrite_req_offset = le64_at(req + 11);
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
    if (type == P9_TLCREATE) {
        // Rlcreate = qid(13) + iounit(4), same shape as Rlopen. path 0x77
        // distinguishes a created file's qid from a walked/opened one.
        size_t total = P9_HDR_LEN + P9_QID_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RLCREATE;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTFILE;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 0x77; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        resp[20] = 0; resp[21] = 0x10; resp[22] = 0; resp[23] = 0;     // iounit=4096
        return (int)total;
    }
    if (type == P9_TMKDIR) {
        // Rmkdir = qid(13). path 0x55 distinguishes a mkdir'd dir.
        size_t total = P9_HDR_LEN + P9_QID_LEN;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RMKDIR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = P9_QTDIR;
        for (int i = 0; i < 4; i++) resp[8 + i] = 0;
        resp[12] = 0x55; for (int i = 1; i < 8; i++) resp[12 + i] = 0;
        return (int)total;
    }
    if (type == P9_TFSYNC) {
        // Rfsync = header only (empty body).
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RFSYNC;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    if (type == P9_TREADDIR) {
        // Capture the request offset (Treaddir body: fid@7, offset@11, count@19).
        if (req_len >= P9_HDR_LEN + 4 + 8 + 4)
            g_readdir_req_offset = le64_at(req + 11);
        // Rreaddir = count(4 LE) + data. One dirent "foo":
        //   qid(13) + offset(8 LE, cookie=1) + dtype(1) + name_len(2 LE=3) + "foo"
        // = 27 bytes.
        const u8 nm[] = {'f','o','o'};
        u32 dcount = (u32)(P9_QID_LEN + 8 + 1 + 2 + sizeof(nm));   // 27
        size_t total = P9_HDR_LEN + 4 + dcount;
        if (resp_cap < total) return -1;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RREADDIR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(dcount & 0xff); resp[8] = (u8)((dcount >> 8) & 0xff);
        resp[9] = 0; resp[10] = 0;
        size_t o = 11;
        // qid (13): type(1) + version(4) + path(8 = 0xAB)
        resp[o++] = P9_QTFILE;
        for (int i = 0; i < 4; i++) resp[o++] = 0;
        resp[o++] = 0xAB; for (int i = 1; i < 8; i++) resp[o++] = 0;
        // offset cookie (8 LE) = 1
        resp[o++] = 1; for (int i = 1; i < 8; i++) resp[o++] = 0;
        // dirent type (1)
        resp[o++] = 8;
        // name_len (2 LE) = 3
        resp[o++] = (u8)sizeof(nm); resp[o++] = 0;
        for (size_t i = 0; i < sizeof(nm); i++) resp[o++] = nm[i];
        return (int)total;
    }
    if (type == P9_TRENAMEAT) {
        // Rrenameat = header only (empty body), like Rfsync.
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RRENAMEAT;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    if (type == P9_TUNLINKAT) {
        // Runlinkat = header only (empty body).
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RUNLINKAT;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    if (type == P9_TGETATTR) {
        // Rgetattr body: valid(8)+qid(13)+mode(4)+uid(4)+gid(4)+15*u64 = 153.
        // Distinguishable mode/uid/gid so the test can prove dev9p_stat_native
        // maps them into struct t_stat.
        size_t total = P9_HDR_LEN + 8 + P9_QID_LEN + 4 + 4 + 4 + 15 * 8;  // 160
        if (resp_cap < total) return -1;
        for (size_t i = 0; i < total; i++) resp[i] = 0;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[4] = P9_RGETATTR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        size_t o = P9_HDR_LEN;
        resp[o] = 0xff; resp[o + 1] = 0x07; o += 8;          // valid = 0x7ff (BASIC)
        resp[o] = P9_QTFILE; o += 1 + 4; resp[o] = 0x33; o += 8;   // qid (path 0x33)
        { u32 m = 0100644u;                                  // mode S_IFREG|0644
          resp[o] = (u8)m; resp[o + 1] = (u8)(m >> 8);
          resp[o + 2] = (u8)(m >> 16); resp[o + 3] = (u8)(m >> 24); o += 4; }
        resp[o] = 0x34; resp[o + 1] = 0x12; o += 4;          // uid = 0x1234
        resp[o] = 0x78; resp[o + 1] = 0x56; o += 4;          // gid = 0x5678
        resp[o] = 1; o += 8;                                 // nlink = 1
        o += 8;                                              // rdev = 0
        resp[o] = 0x00; resp[o + 1] = 0x01; o += 8;          // size = 0x100
        resp[o] = 0x00; resp[o + 1] = 0x10; o += 8;          // blksize = 4096
        resp[o] = 1; o += 8;                                 // blocks = 1
        o += 8 * 8 + 2 * 8;                                  // a/m/c/b times + gen + dv = 0
        (void)o;                                             // o == total here
        return (int)total;
    }
    if (type == P9_TWALKGETATTR) {
        // POUNCE: Rwalkgetattr = nwqid(2) + nwqid * getattr_body(153). Echo
        // the requested component count (or one fewer under g_wga_partial --
        // the session layer then leaves newfid unbound). Body offsets:
        // hdr(7) + fid(4) + newfid(4) + request_mask(8) = 23; nwname at 23.
        if (req_len < P9_HDR_LEN + 4 + 4 + 8 + 2) return -1;
        u16 nwname = (u16)req[23] | ((u16)req[24] << 8);
        u16 nwqid  = nwname;
        if (g_dev9p_wga_partial && nwqid > 0) nwqid--;
        size_t total = P9_HDR_LEN + 2 + (size_t)nwqid * P9_WGA_BODY_LEN;
        if (resp_cap < total) return -1;
        for (size_t i = 0; i < total; i++) resp[i] = 0;
        resp[0] = (u8)(total & 0xff); resp[1] = (u8)((total >> 8) & 0xff);
        resp[4] = P9_RWALKGETATTR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(nwqid & 0xff); resp[8] = (u8)((nwqid >> 8) & 0xff);
        for (u16 e = 0; e < nwqid; e++) {
            size_t o = P9_HDR_LEN + 2 + (size_t)e * P9_WGA_BODY_LEN;
            resp[o] = 0xff; resp[o + 1] = 0x07;              // valid = BASIC
            o += 8;
            resp[o] = P9_QTFILE; o += 1 + 4;                 // qid.type + version
            resp[o] = (u8)(0x20 + e); o += 8;                // qid.path
            { u32 m = 0100644u;                              // mode
              resp[o] = (u8)m; resp[o + 1] = (u8)(m >> 8);
              resp[o + 2] = (u8)(m >> 16); resp[o + 3] = (u8)(m >> 24); o += 4; }
            resp[o] = 0x11; resp[o + 1] = 0x11; o += 4;      // uid = 0x1111
            resp[o] = 0x22; resp[o + 1] = 0x22; o += 4;      // gid = 0x2222
            resp[o] = 1; o += 8;                             // nlink
            o += 8;                                          // rdev
            resp[o] = (u8)e; resp[o + 1] = 0x02; o += 8;     // size = 0x200 + e
            resp[o] = 0x00; resp[o + 1] = 0x10; o += 8;      // blksize = 4096
            // blocks + times + gen + dv stay 0.
        }
        return (int)total;
    }
    if (type == P9_TSETATTR) {
        // Capture the request's valid/mode/uid/gid (Tsetattr body: fid@7,
        // valid@11, mode@15, uid@19, gid@23) so the test asserts the
        // T_WSTAT_* mask + values reached the wire.
        if (req_len >= P9_HDR_LEN + 4 + 4 + 4 + 4 + 4) {
            g_setattr_valid = le32_at(req + 11);
            g_setattr_mode  = le32_at(req + 15);
            g_setattr_uid   = le32_at(req + 19);
            g_setattr_gid   = le32_at(req + 23);
        }
        // Rsetattr = header only (empty body).
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RSETATTR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    return -1;
}

// Helper: drive a client through handshake against the canonical
// responder, returning a Spoor at the bound root.
static struct Spoor *make_open_client_and_root(void) {
    if (p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                            dev9p_responder, NULL) != 0) return NULL;
    if (p9_client_init(&g_client, /*root_fid=*/0, 8192,
                          p9_loopback_ops_for(&g_loopback),
                          g_recv_buf, sizeof(g_recv_buf)) != 0) return NULL;
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    if (p9_client_handshake(&g_client, uname, sizeof(uname),
                              aname, sizeof(aname), 0) != 0) return NULL;
    return dev9p_attach_client(&g_client, /*root_fid=*/0);
}

// Tear down helper: release the root Spoor + destroy the client.
static void teardown(struct Spoor *root) {
    spoor_clunk(root);
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// Tests.
// =============================================================================

void test_dev9p_registered(void) {
    struct Dev *d = dev_lookup_by_dc(DEV9P_DC);
    TEST_ASSERT(d == &dev9p,
                 "dev9p registered in bestiary with dc='9'");
    TEST_ASSERT(d->walk != NULL && d->open != NULL && d->read != NULL &&
                 d->write != NULL && d->close != NULL,
                 "dev9p vtable slots are populated");
}

void test_dev9p_attach_client_root_spoor(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL,             "root Spoor allocated");
    TEST_EXPECT_EQ((u64)root->dc, (u64)DEV9P_DC, "root Spoor's dc = '9'");
    TEST_ASSERT(root->aux != NULL,        "root has aux populated");
    teardown(root);
}

void test_dev9p_walk_one_component(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");

    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");

    const char *name = "etc";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL, "walk returned a Walkqid");
    TEST_EXPECT_EQ((u64)w->nqid, (u64)1, "1 qid back");
    TEST_ASSERT(w->spoor == nc, "Walkqid carries nc");
    TEST_ASSERT(nc->aux != NULL && nc->aux != root->aux,
                "nc has its own priv (not shared with root)");

    walkqid_free(w);
    spoor_clunk(nc);
    teardown(root);
}

void test_dev9p_walk_clone(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");

    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");

    // nname=0 → walk-clone shape (no path components).
    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);
    TEST_ASSERT(w != NULL, "clone walk returned Walkqid");
    TEST_EXPECT_EQ((u64)w->nqid, (u64)0, "clone returns 0 qids");
    TEST_ASSERT(nc->aux != NULL && nc->aux != root->aux,
                "nc has its own priv");

    walkqid_free(w);
    spoor_clunk(nc);
    teardown(root);
}

void test_dev9p_open_lopens_fid(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL, "walk to file");
    walkqid_free(w);

    struct Spoor *opened = dev9p.open(nc, 0);
    TEST_ASSERT(opened == nc,                  "open returns the Spoor");
    TEST_ASSERT((opened->flag & COPEN) != 0,   "COPEN flag set");
    TEST_EXPECT_EQ((u64)opened->qid.path, (u64)0x42,
                    "open updated qid from Rlopen response");

    spoor_clunk(nc);
    teardown(root);
}

void test_dev9p_read_routes_through_client(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);
    dev9p.open(nc, 0);

    u8 buf[64];
    long got = dev9p.read(nc, buf, 64, 0);
    TEST_EXPECT_EQ((u64)got, (u64)5,                  "read returned 5 bytes");
    TEST_ASSERT(buf[0] == 'h' && buf[4] == 'o',       "payload matches");

    spoor_clunk(nc);
    teardown(root);
}

void test_dev9p_write_routes_through_client(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);
    dev9p.open(nc, 1);

    const u8 payload[] = {'w','r','i','t','e'};
    long acc = dev9p.write(nc, payload, (long)sizeof(payload), 0);
    TEST_EXPECT_EQ((u64)acc, (u64)sizeof(payload),
                    "write returned full accepted count");

    spoor_clunk(nc);
    teardown(root);
}

// #3 (Area F errno-rollout) regression: a 9P write/read error (Rlerror)
// must surface as the real negative -errno through dev9p, NOT collapse to
// -1 (== -EPERM, which mis-reported every Stratum write failure as a
// permission error -- the go-build's STM_ENOSPC -> EPERM cascade).
// g_io_error_ecode, when nonzero, makes the responder answer every
// Twrite/Tread with Rlerror(ecode); the handshake/walk/open/clunk ops
// delegate to the canonical responder so the fid is openable first.
static u32 g_io_error_ecode;

static int dev9p_io_error_responder(void *ctx, const u8 *req, size_t req_len,
                                     u8 *resp, size_t resp_cap) {
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if ((type == P9_TWRITE || type == P9_TREAD) && g_io_error_ecode != 0) {
        // Rlerror body: [ecode:u32]. Frame = [size:4][type=7][tag:2][ecode:4].
        size_t total = P9_HDR_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = (u8)total; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RLERROR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = (u8)(g_io_error_ecode & 0xff);
        resp[8] = (u8)((g_io_error_ecode >> 8) & 0xff);
        resp[9] = (u8)((g_io_error_ecode >> 16) & 0xff);
        resp[10] = (u8)((g_io_error_ecode >> 24) & 0xff);
        return (int)total;
    }
    return dev9p_responder(ctx, req, req_len, resp, resp_cap);
}

void test_dev9p_write_read_propagate_errno(void) {
    g_io_error_ecode = 0;
    TEST_ASSERT(p9_loopback_init(&g_loopback, g_loopback_resp,
                                  sizeof(g_loopback_resp),
                                  dev9p_io_error_responder, NULL) == 0,
                 "loopback init (error responder)");
    TEST_ASSERT(p9_client_init(&g_client, /*root_fid=*/0, 8192,
                                p9_loopback_ops_for(&g_loopback),
                                g_recv_buf, sizeof(g_recv_buf)) == 0,
                 "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_ASSERT(p9_client_handshake(&g_client, uname, sizeof(uname),
                                      aname, sizeof(aname), 0) == 0,
                 "handshake");
    struct Spoor *root = dev9p_attach_client(&g_client, /*root_fid=*/0);
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);
    dev9p.open(nc, 0);

    // ENOSPC (28) -- the exact go-build `_pkg_.a` exhaustion ecode. Pre-#3
    // dev9p.write collapsed any rc!=0 to -1 (== -EPERM); post-#3 it
    // propagates the real -errno so userspace sees ENOSPC.
    g_io_error_ecode = 28u;     // ENOSPC
    u8 wbuf[8] = {0};
    long wr = dev9p.write(nc, wbuf, 8, 0);
    TEST_EXPECT_EQ((u64)wr, (u64)(-(long)28),
                    "dev9p.write propagates -ENOSPC (not the -1 collapse)");

    // EIO (5 == T_E_IO) on the read twin (dev9p_read had the same collapse).
    g_io_error_ecode = 5u;      // EIO
    u8 rbuf[8];
    long rd = dev9p.read(nc, rbuf, 8, 0);
    TEST_EXPECT_EQ((u64)rd, (u64)(-(long)5),
                    "dev9p.read propagates -EIO (not the -1 collapse)");

    g_io_error_ecode = 0;
    spoor_clunk(nc);
    teardown(root);
}

void test_dev9p_close_clunks_owned_fid(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);

    // Capture inflight count before close (should be 0 — walk completed).
    size_t before = p9_session_inflight(&g_client.session);
    spoor_clunk(nc);
    // After close, the dev9p.close path issued a Tclunk + got Rclunk
    // synchronously through the loopback. Inflight should still be 0.
    size_t after = p9_session_inflight(&g_client.session);
    TEST_EXPECT_EQ((u64)before, (u64)after,
                    "clunk completed synchronously (inflight unchanged)");
    // The walk-allocated fid is no longer in the session's bound set.
    // The walk allocated fid 1 (next_fid after root_fid=0); verify it's
    // not bound anymore.
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 1),
                 "walk-allocated fid 1 is unbound after close");

    teardown(root);
}

void test_dev9p_close_does_not_clunk_root_fid(void) {
    struct Spoor *root = make_open_client_and_root();
    // Root's fid_owned is false; closing the root Spoor must not
    // clunk root_fid (the higher layer owns the lifecycle).
    TEST_ASSERT(p9_session_fid_bound(&g_client.session, 0),
                 "root fid bound after handshake");
    spoor_clunk(root);
    TEST_ASSERT(p9_session_fid_bound(&g_client.session, 0),
                 "root fid STILL bound after closing root Spoor "
                 "(close did NOT clunk; higher layer owns root_fid)");
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// dev9p.create file path (FS-mutation foundation, SYS_WALK_CREATE). Mirrors
// what sys_walk_create_handler does: clone the parent dir Spoor, clone-walk it
// so the clone carries its own fid at the parent, then create. dev9p_create
// drives Tlcreate (the responder's 0x77 qid) which both creates AND opens.
void test_dev9p_create_file(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");

    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);   // clone-walk
    TEST_ASSERT(w != NULL, "clone-walk gave nc its own fid");
    walkqid_free(w);

    struct Spoor *opened = dev9p.create(nc, "newfile", 1 /*OWRITE*/, 0644u, 1000u);
    TEST_ASSERT(opened == nc,                  "create(file) returns the Spoor");
    TEST_ASSERT((nc->flag & COPEN) != 0,       "COPEN set after Tlcreate");
    TEST_EXPECT_EQ((u64)nc->qid.path, (u64)0x77,
                    "qid taken from the Rlcreate response");

    spoor_clunk(nc);
    teardown(root);
}

// L1c regression (the create-reuse stale-serve, the stalk-2-e2e failure): a
// create at a qid.path that carries a STALE cached attr -- Stratum reused a
// just-freed ino, so the new file's qid.path had a prior occupant cached -- MUST
// invalidate that child, or the base X-check on the new file serves the old
// file's attr. The create path never runs walk_attrs (no revalidate-by-
// overwrite), so dev9p_create must invalidate the child explicitly. The
// responder mints 0x77 for the created file; pre-seed a bogus 0x77 entry and
// verify create drops it. FAILS pre-fix (create invalidated only the parent).
void test_dev9p_create_invalidates_reused_child(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");
    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);
    TEST_ASSERT(w != NULL, "clone-walk");
    walkqid_free(w);

    // Pre-seed a stale attr at qid.path 0x77 (the created file's qid), as if a
    // prior file at that reused ino was cached -- a FILE mode (no X-bit), which a
    // base X-check on the new dir/file would wrongly consult.
    struct t_stat stale;
    for (size_t i = 0; i < sizeof(stale); i++) ((u8 *)&stale)[i] = 0;
    stale.mode = 0644;
    larder_attr_install(&g_client.larder, larder_gen_snapshot(&g_client.larder),
                        0x77, /*cvers=*/1, &stale);
    struct t_stat probe; u64 s0 = 0;
    TEST_ASSERT(larder_attr_serve(&g_client.larder, 0x77, &probe, &s0),
                "stale child entry present pre-create");

    struct Spoor *opened = dev9p.create(nc, "newfile", 1 /*OWRITE*/, 0644u, 1000u);
    TEST_ASSERT(opened == nc, "create returns the Spoor");
    TEST_EXPECT_EQ((u64)nc->qid.path, (u64)0x77, "qid from Rlcreate");
    TEST_ASSERT(!larder_attr_serve(&g_client.larder, 0x77, &probe, &s0),
                "create invalidates the reused child's stale attr");

    spoor_clunk(nc);
    teardown(root);
}

// dev9p.create directory path: perm & DMDIR -> Tmkdir, then walk+lopen the new
// dir so the returned Spoor is opened OREAD on it.
void test_dev9p_create_dir(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");

    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);   // clone-walk
    TEST_ASSERT(w != NULL, "clone-walk");
    walkqid_free(w);

    // 0x80000000 == DMDIR (SYS_WALK_CREATE_DMDIR); omode OREAD for a dir.
    struct Spoor *opened = dev9p.create(nc, "newdir", 0 /*OREAD*/,
                                         0x80000000u | 0755u, 1000u);
    TEST_ASSERT(opened == nc,             "create(dir) returns the Spoor");
    TEST_ASSERT((nc->flag & COPEN) != 0,  "COPEN set after Tmkdir+walk+lopen");

    spoor_clunk(nc);
    teardown(root);
}

// dev9p.fsync routes through p9_client_fsync -> Tsync; the responder Rfsync
// makes it return 0 (durability barrier; FS-mutation foundation).
void test_dev9p_fsync(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.fsync != NULL, "dev9p has .fsync slot");
    TEST_EXPECT_EQ((u64)dev9p.fsync(root, 0), (u64)0, "fsync(full) -> 0");
    TEST_EXPECT_EQ((u64)dev9p.fsync(root, 1), (u64)0, "fsync(datasync) -> 0");
    teardown(root);
}

// dev9p.readdir routes through p9_client_readdir -> Treaddir; the responder
// returns one 27-byte dirent ("foo"). Verifies the raw 9P2000.L dirent stream
// reaches the caller (the SYS_READDIR handler does the cookie-advance parse).
void test_dev9p_readdir(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.readdir != NULL, "dev9p has .readdir slot");

    u8 buf[128];
    long got = dev9p.readdir(root, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ((u64)got, (u64)27, "readdir returned the 27-byte dirent run");
    // Layout: qid(13) + offset(8) + dtype(1) + name_len(2 LE @22) + name(@24).
    TEST_EXPECT_EQ((u64)buf[22], (u64)3, "dirent name_len == 3");
    TEST_ASSERT(buf[24] == 'f' && buf[25] == 'o' && buf[26] == 'o',
                "dirent name is 'foo'");
    teardown(root);
}

// #955 regression: the Treaddir `offset` is an OPAQUE resume cookie. Stratum's
// cookies for real dirents are hash-derived u64 values that routinely exceed
// INT64_MAX (bit 63 set); the kernel carries the cookie through the s64
// Spoor.offset, so dev9p_readdir MUST reinterpret the bits straight back to u64
// rather than sign-clamping a "negative" value to 0. Pre-fix, the clamp restarted
// enumeration at 0 -> a paginating reader re-fetched the first batch forever.
void test_dev9p_readdir_cookie_high_bit(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");

    u8 buf[128];
    // A cookie with bit 63 set: as an s64 this is negative; the pre-fix clamp
    // would forward 0 on the wire instead.
    s64 hi_cookie = (s64)0xC123456789ABCDEFULL;
    g_readdir_req_offset = 0xdeadbeefu;   // sentinel; the responder overwrites it
    long got = dev9p.readdir(root, buf, (long)sizeof(buf), hi_cookie);
    TEST_EXPECT_EQ((u64)got, (u64)27, "readdir still returns the dirent run");
    TEST_EXPECT_EQ(g_readdir_req_offset, 0xC123456789ABCDEFULL,
                   "dev9p_readdir forwards the high-bit cookie verbatim (no sign-clamp)");
    teardown(root);
}

// dev9p.rename routes through p9_client_renameat -> Trenameat; the responder
// Rrenameat makes it return 0. olddir == newdir == root exercises the same-Dev
// same-session path (the common case -- corvus's identity.db tmp->real swap).
void test_dev9p_rename(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.rename != NULL, "dev9p has .rename slot");
    TEST_EXPECT_EQ((u64)dev9p.rename(root, "old", root, "new"), (u64)0,
                    "rename(old -> new) within one dir -> 0");
    teardown(root);
}

// dev9p.unlink routes through p9_client_unlinkat -> Tunlinkat; the responder
// Runlinkat makes it return 0. Both the file (flags 0) and the rmdir
// (P9_UNLINK_AT_REMOVEDIR) flag values reach the wire intact.
void test_dev9p_unlink(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.unlink != NULL, "dev9p has .unlink slot");
    TEST_EXPECT_EQ((u64)dev9p.unlink(root, "victim", 0u), (u64)0,
                    "unlink(file) -> 0");
    TEST_EXPECT_EQ((u64)dev9p.unlink(root, "emptydir", P9_UNLINK_AT_REMOVEDIR),
                    (u64)0, "unlink(REMOVEDIR) -> 0");
    teardown(root);
}

// A-2a: dev9p_stat_native maps the server's Rgetattr (mode/uid/gid/size)
// into struct t_stat -- the metadata source SYS_FSTAT + the kernel rwx layer
// (A-2d) read for a Stratum-backed file.
void test_dev9p_stat_native_maps_getattr(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.stat_native != NULL, "dev9p has .stat_native slot");
    struct t_stat st;
    TEST_EXPECT_EQ((u64)dev9p.stat_native(root, &st), (u64)0,
                    "stat_native -> 0 (Rgetattr)");
    TEST_EXPECT_EQ((u64)st.uid, (u64)0x1234u, "uid from Rgetattr");
    TEST_EXPECT_EQ((u64)st.gid, (u64)0x5678u, "gid from Rgetattr");
    TEST_EXPECT_EQ((u64)st.mode, (u64)0100644u, "mode from Rgetattr");
    TEST_EXPECT_EQ((u64)st.size, (u64)0x100u, "size from Rgetattr");
    teardown(root);
}

// A-2a: dev9p_wstat_native maps the T_WSTAT_* mask + (mode, uid, gid) onto a
// Tsetattr that reaches the wire intact -- the chmod/chown write path. Proves
// the T_WSTAT_* == P9_SETATTR_* no-translation map.
void test_dev9p_wstat_native_drives_setattr(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.wstat_native != NULL, "dev9p has .wstat_native slot");
    g_setattr_valid = g_setattr_mode = g_setattr_uid = g_setattr_gid = 0xdeadbeefu;
    TEST_EXPECT_EQ((u64)dev9p.wstat_native(root,
                       T_WSTAT_MODE | T_WSTAT_UID | T_WSTAT_GID,
                       0640u, 1000u, 2000u), (u64)0,
                    "wstat_native -> 0 (Rsetattr)");
    TEST_EXPECT_EQ((u64)g_setattr_valid,
                    (u64)(T_WSTAT_MODE | T_WSTAT_UID | T_WSTAT_GID),
                    "valid mask reached the wire");
    TEST_EXPECT_EQ((u64)g_setattr_mode, (u64)0640u, "mode on the wire");
    TEST_EXPECT_EQ((u64)g_setattr_uid, (u64)1000u, "uid on the wire");
    TEST_EXPECT_EQ((u64)g_setattr_gid, (u64)2000u, "gid on the wire");
    teardown(root);
}

// A-3b: a synthetic Proc carrying only the identity perm_check reads.
static void mkproc9(struct Proc *p, u32 principal, u32 pgid) {
    for (size_t i = 0; i < sizeof(*p); i++) ((u8 *)p)[i] = 0;
    p->principal_id   = principal;
    p->primary_gid    = pgid;
    p->supp_gid_count = 0;
    p->caps           = CAP_NONE;
}

// A-3b: dev9p enforcement composes dev9p_stat_native (the loopback Rgetattr:
// uid 0x1234, gid 0x5678, mode 0100644 = -rw-r--r--) with perm_check. Proves the
// wiring on dev9p SPECIFICALLY (devramfs is the A-2d test) reads the right
// owner/group/other bits, and that PRINCIPAL_SYSTEM gets NO ambient bypass (I-22).
void test_dev9p_perm_enforced_deny_allow(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    TEST_ASSERT(dev9p.perm_enforced, "dev9p enforces rwx (A-3b)");
    struct t_stat st;
    TEST_EXPECT_EQ((u64)dev9p.stat_native(root, &st), (u64)0, "stat_native -> 0");

    struct Proc owner, grp, other, sys;
    mkproc9(&owner, 0x1234u, 0x9999u);              // owner == st.uid
    mkproc9(&grp,   0x4321u, 0x5678u);              // member of st.gid
    mkproc9(&other, 0x4321u, 0x9999u);              // neither owner nor group
    mkproc9(&sys,   PRINCIPAL_SYSTEM, GID_SYSTEM);  // system, but not owner/group

    TEST_EXPECT_EQ(perm_check(&owner, &st, PERM_W), 0,  "dev9p owner W allowed (rw-)");
    TEST_EXPECT_EQ(perm_check(&grp,   &st, PERM_R), 0,  "dev9p group R allowed (r--)");
    TEST_EXPECT_EQ(perm_check(&grp,   &st, PERM_W), -1, "dev9p group W denied");
    TEST_EXPECT_EQ(perm_check(&other, &st, PERM_R), 0,  "dev9p other R allowed (r--)");
    TEST_EXPECT_EQ(perm_check(&other, &st, PERM_W), -1, "dev9p other W denied");
    TEST_EXPECT_EQ(perm_check(&sys,   &st, PERM_W), -1,
                   "dev9p PRINCIPAL_SYSTEM W denied (I-22: no identity bypass)");
    teardown(root);
}

// net-6b-2b: the dev9p.poll QTPOLL gate -- the soundness-critical distinguisher.
// A regular dev9p file (no QTPOLL on its cached qid; the root is QTDIR) is POSIX
// always-ready and must NEVER be probed: dev9p_poll returns events & requestable
// WITHOUT allocating a poll-state or submitting a readiness Tread. (The QTPOLL
// PROBE path -- submit -> poll-pump kthread -> netd reply -> wake -- is exercised
// end-to-end by the joey net-6b in-guest probe against netd's real `ready` file;
// a unit test would drive the live poll-pump kthread against this loopback client.)
void test_dev9p_poll_regular_file_always_ready(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    // root->qid.type is QTDIR -- no QTPOLL. The gate -> POSIX always-ready.
    short rv = dev9p_poll(root, (short)(POLLIN | POLLOUT), NULL);
    TEST_EXPECT_EQ((u64)(rv & (POLLIN | POLLOUT)), (u64)(POLLIN | POLLOUT),
                   "non-QTPOLL dev9p file is always-ready (POLLIN|POLLOUT)");
    struct dev9p_priv *p = dev9p_priv_of(root);
    TEST_ASSERT(p != NULL && p->poll == NULL,
                "no poll-state allocated for a regular (non-QTPOLL) file");
    // priv_release on a NULL-poll priv is a safe no-op (idempotent with the
    // dev9p_close call teardown() triggers).
    dev9p_poll_priv_release(p);
    TEST_ASSERT(p->poll == NULL, "priv_release on NULL poll is a no-op");
    teardown(root);
}

// =============================================================================
// #294 cancel-at-close: a readiness op outstanding at dev9p_close.
// =============================================================================

static u32  g_cancel_treads;        // readiness Treads the responder saw
static u32  g_cancel_tflushes;      // cancel-at-close Tflushes the responder saw
static u32  g_cancel_tclunks;       // Tclunks the responder saw
static u32  g_cancel_ready_fid;     // the fid the readiness Tread + close Tclunk name
static bool g_cancel_ready_clunked; // the readiness fid's Tclunk was delivered

// A responder for the cancel-at-close test: the walked file is QTPOLL (so
// dev9p_poll probes it) and its readiness Tread DEFERS (stages no reply, so the op
// stays outstanding). The Tflush (the cancel) stages NOTHING -- p9_client_abandon_
// async does not await an Rflush, and a staged-undrained reply would block the
// synchronous Tclunk that follows (loopback_send refuses a second send while a
// reply is undrained). The Tclunk -- the leak fix's deliverable -- replies Rclunk
// and is recorded.
static int dev9p_cancel_responder(void *ctx, const u8 *req, size_t req_len,
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
            resp[off] = (u8)(P9_QTFILE | P9_QTPOLL);   // QTPOLL => the readiness file
            for (int j = 0; j < 4; j++) resp[off + 1 + j] = 0;
            resp[off + 5] = (u8)(0x10 + i);
            for (int j = 1; j < 8; j++) resp[off + 5 + j] = 0;
        }
        return (int)total;
    }
    if (type == P9_TREAD) {
        // The readiness probe (Tread offset=mask). DEFER: stage NO reply so the op
        // stays outstanding. Record the fid for the Tclunk assertion.
        g_cancel_treads++;
        if (req_len >= P9_HDR_LEN + 4) g_cancel_ready_fid = le32_at(req + 7);
        return 0;
    }
    if (type == P9_TFLUSH) {
        // The cancel-at-close Tflush. Stage NOTHING (see the responder comment).
        g_cancel_tflushes++;
        return 0;
    }
    if (type == P9_TCLUNK) {
        // The `ready`-fd Tclunk -- the leak fix's deliverable.
        g_cancel_tclunks++;
        if (req_len >= P9_HDR_LEN + 4 && le32_at(req + 7) == g_cancel_ready_fid)
            g_cancel_ready_clunked = true;
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RCLUNK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    return -1;
}

// #294 cancel-at-close regression. A readiness op outstanding at dev9p_close must
// be CANCELLED (Tflush) and the `ready`-fd Tclunk delivered -- not extinct (the
// pre-#294 dev9p_poll_priv_release extincted on a live op) and not leak (the bug:
// the kthread-GC-deferred Tclunk could never reach netd, leaving the slot pinned).
// Deterministic: in the in-kernel suite SMP + preemption are off and the test
// thread does not yield between submit and close, so the close -- not the poll-pump
// kthread -- grabs + cancels the op. attached_owner==NULL here (the test path); the
// session-ref leg is exercised by the live netd boot probes.
void test_dev9p_poll_cancel_at_close(void) {
    g_cancel_treads = 0; g_cancel_tflushes = 0; g_cancel_tclunks = 0;
    g_cancel_ready_fid = 0; g_cancel_ready_clunked = false;

    TEST_ASSERT(p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                                 dev9p_cancel_responder, NULL) == 0, "loopback init");
    TEST_ASSERT(p9_client_init(&g_client, /*root_fid=*/0, 8192,
                               p9_loopback_ops_for(&g_loopback),
                               g_recv_buf, sizeof(g_recv_buf)) == 0, "client init");
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    TEST_ASSERT(p9_client_handshake(&g_client, uname, sizeof(uname),
                                    aname, sizeof(aname), 0) == 0, "handshake");
    struct Spoor *root = dev9p_attach_client(&g_client, /*root_fid=*/0);
    TEST_ASSERT(root != NULL, "root");

    // Walk to a QTPOLL file (a netd `ready` analog) -- a fid_owned walked Spoor,
    // so its close issues a Tclunk (the leak fix's deliverable).
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");
    const char *name = "ready";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL && w->spoor == nc, "walk to the readiness file");
    walkqid_free(w);
    TEST_ASSERT((nc->qid.type & QTPOLL) != 0, "the walked qid carries QTPOLL");

    u32 base = dev9p_poll_op_count_for_test();

    // Poll: the responder defers the readiness Tread, so dev9p_poll submits an op +
    // returns not-ready, leaving it OUTSTANDING (the timed-out-poll state). NOTE: a
    // live op-COUNT snapshot here would race the poll-pump kthread, which runs on a
    // secondary CPU during the suite (SMP is up) and can stranded-GC a pw==NULL op
    // before this thread reads the count -- so we assert the NON-racy submission
    // witness (the Tread reached the responder, recorded synchronously during the
    // submit) instead. The post-close assertions below hold whether the close OR the
    // kthread tore the op down (both deliver the Tflush + leave the Tclunk to close).
    short rv = dev9p_poll(nc, (short)POLLIN, NULL);
    TEST_EXPECT_EQ((u64)(rv & POLLIN), (u64)0, "deferred readiness -> not ready");
    TEST_EXPECT_EQ((u64)g_cancel_treads, (u64)1, "one readiness Tread submitted");

    struct dev9p_priv *ncp = dev9p_priv_of(nc);
    TEST_ASSERT(ncp != NULL && ncp->fid_owned, "the walked Spoor owns its fid");
    u32 want_fid = ncp->fid;

    // Close the readiness Spoor WITH the op still live. Pre-#294 this extincted in
    // dev9p_poll_priv_release; now it cancels the op (Tflush) + delivers the Tclunk.
    spoor_clunk(nc);

    TEST_EXPECT_EQ((u64)dev9p_poll_op_count_for_test(), (u64)base,
                   "the live op was torn down at close (registry back to baseline)");
    TEST_ASSERT(g_cancel_tflushes >= 1, "the outstanding op was cancelled (Tflush)");
    // The `ready`-fd Tclunk reached the server -> netd's slot_unref fires (the leak
    // fix). Note the Tflush leaves the readiness tag awaiting_flush; the clunk
    // succeeds because any_outstanding_on_fid no longer counts a flushed op (the
    // 9p_session SendClunk-precondition fix this test surfaced).
    TEST_ASSERT(g_cancel_ready_clunked,
                "the `ready`-fd Tclunk was delivered at close (#294 leak fix)");
    TEST_EXPECT_EQ((u64)p9_session_fid_bound(&g_client.session, want_fid), (u64)0,
                   "the readiness fid is unbound after close (clunk completed)");

    spoor_clunk(root);   // root_fid is not clunked by dev9p (fid_owned false); frees the Spoor
    p9_client_destroy(&g_client);
    p9_loopback_destroy(&g_loopback);
}

// =============================================================================
// SYS_PREAD / SYS_PWRITE (#37) + SYS_WSTAT kind-gate (#47) — syscall-layer
// tests against the loopback client (the wire-visible halves).
// =============================================================================

// The positioned inners + the wstat inner (defined in kernel/syscall.c).
extern s64 sys_pread_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len,
                              s64 off);
extern s64 sys_pwrite_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                               u64 len, s64 off);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);
extern s64 sys_wstat_for_proc(struct Proc *p, hidx_t h, u32 valid, u32 mode,
                              u32 uid, u32 gid);

// #37: SYS_PREAD/SYS_PWRITE put the CALLER's offset in the Tread/Twrite and
// never move the Spoor cursor; SYS_READ/SYS_WRITE keep putting the CURSOR
// there and advancing it. The loopback responder captures the wire offsets.
void test_dev9p_prw_wire_offset_and_cursor(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "client+root");
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL, "walk");
    walkqid_free(w);
    TEST_ASSERT(dev9p.open(nc, 2) != NULL, "open ORDWR");

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, nc);
    TEST_ASSERT(fd >= 0, "handle_alloc");   // adopts nc's ref

    const u8 payload[5] = {'w','r','i','t','e'};
    u8 buf[8];

    g_twrite_req_offset = 0xdeadbeefull;
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fd, payload, 5, 0x1234), 5L,
                   "pwrite accepted");
    TEST_EXPECT_EQ(g_twrite_req_offset, 0x1234ull,
                   "Twrite carries the caller's offset");
    TEST_EXPECT_EQ((s64)nc->offset, 0LL, "cursor untouched by pwrite");

    g_tread_req_offset = 0xdeadbeefull;
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 5, 0x77), 5L,
                   "pread returned payload");
    TEST_EXPECT_EQ(g_tread_req_offset, 0x77ull,
                   "Tread carries the caller's offset");
    TEST_EXPECT_EQ((s64)nc->offset, 0LL, "cursor untouched by pread");

    // The cursor path interleaves cleanly: a plain write goes at the cursor
    // (0) and advances it; a positioned write elsewhere leaves it; the next
    // plain write continues from 5.
    TEST_EXPECT_EQ(sys_write_for_proc(p, fd, payload, 5), 5L, "cursor write");
    TEST_EXPECT_EQ(g_twrite_req_offset, 0ull, "cursor write went at 0");
    TEST_EXPECT_EQ((s64)nc->offset, 5LL, "cursor advanced to 5");
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fd, payload, 5, 9), 5L, "pwrite @9");
    TEST_EXPECT_EQ(g_twrite_req_offset, 9ull, "Twrite offset 9");
    TEST_EXPECT_EQ((s64)nc->offset, 5LL, "cursor still 5 after pwrite");
    TEST_EXPECT_EQ(sys_write_for_proc(p, fd, payload, 5), 5L, "cursor write 2");
    TEST_EXPECT_EQ(g_twrite_req_offset, 5ull,
                   "second cursor write continued at 5");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);        // releases the handle -> clunks nc's fid
    teardown(root);
}

// #47: SYS_WSTAT is kind-gated only. POSIX fchmod(2) works on an fd opened
// O_RDONLY -- the write authority is perm_wstat_check (the IDENTITY axis),
// not the handle rights. The loopback Rgetattr reports uid 0x1234: an
// owner-principal Proc chmods through a RIGHT_READ-only handle (pre-#47 this
// returned -1 at the rights gate); a stranger holding FULL rights is still
// denied by the identity gate -- the rights-drop opened no authority hole.
void test_dev9p_wstat_readonly_fd(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "client+root");
    const char *name = "file";

    struct Spoor *nc = spoor_clone(root);
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    TEST_ASSERT(w != NULL, "walk");
    walkqid_free(w);

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->principal_id = 0x1234u;     // the owner the loopback Rgetattr reports
    p->primary_gid  = 0x5678u;
    p->caps         = 0;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, nc);
    TEST_ASSERT(fd >= 0, "handle_alloc R-only");

    g_setattr_valid = g_setattr_mode = 0xdeadbeefu;
    TEST_EXPECT_EQ(sys_wstat_for_proc(p, fd, T_WSTAT_MODE, 0644u, 0, 0), 0L,
                   "owner chmod through a READ-only handle succeeds (#47)");
    TEST_EXPECT_EQ(g_setattr_mode, 0644u, "mode reached the wire");

    // The identity gate is the live authority: a non-owner without caps is
    // denied even holding R|W rights.
    struct Spoor *nc2 = spoor_clone(root);
    struct Walkqid *w2 = dev9p.walk(root, nc2, &name, 1);
    TEST_ASSERT(w2 != NULL, "walk 2");
    walkqid_free(w2);
    struct Proc *q = proc_alloc();
    TEST_ASSERT(q != NULL, "proc_alloc q");
    q->principal_id = 0x9999u;
    q->primary_gid  = 0x9999u;
    q->caps         = 0;
    hidx_t fq = handle_alloc(q, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, nc2);
    TEST_ASSERT(fq >= 0, "handle_alloc q");
    TEST_EXPECT_EQ(sys_wstat_for_proc(q, fq, T_WSTAT_MODE, 0600u, 0, 0), -1L,
                   "non-owner chmod denied by perm_wstat_check despite R|W");

    q->state = PROC_STATE_ZOMBIE;
    proc_free(q);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
    teardown(root);
}

// POUNCE (docs/POUNCE-DESIGN.md §4): dev9p.walk_attrs -> Twalkgetattr. The
// wire mechanics were proven at P-2 (test_9p_client walkgetattr); this test
// pins the VTABLE layer: the p9_attr -> t_stat conversion parity with
// dev9p_stat_native (one shared converter), the bind/partial/query fid
// discipline, and the strict nc contract.
void test_dev9p_walk_attrs(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "client + root");
    TEST_ASSERT(dev9p.walk_attrs != NULL, "slot wired");

    const char  *names[2] = { "dirA", "fileB" };
    const size_t lens[2]  = { 4, 5 };

    // BIND form, FULL walk: nc transitions (own fid, leaf qid), and the
    // fused attrs map exactly as dev9p_stat_native maps an Rgetattr
    // (mode/uid/gid under the valid mask; size; qid).
    {
        g_dev9p_wga_partial = false;
        struct Spoor *nc = spoor_clone(root);
        TEST_ASSERT(nc != NULL, "clone");
        struct t_stat sts[2];
        struct Walkqid *w = dev9p.walk_attrs(root, nc, names, lens, 2, sts);
        TEST_ASSERT(w != NULL, "full bind walk");
        TEST_EXPECT_EQ(w->nqid, 2, "both components walked");
        TEST_ASSERT(w->spoor == nc, "nc transitioned");
        TEST_EXPECT_EQ(nc->qid.path, (u64)0x21, "nc at the leaf qid (0x20+1)");
        TEST_EXPECT_EQ((u64)sts[0].qid_path, (u64)0x20, "element 0 qid");
        TEST_EXPECT_EQ((u64)sts[1].qid_path, (u64)0x21, "element 1 qid");
        TEST_EXPECT_EQ((u64)sts[1].mode, (u64)0100644u, "mode mapped");
        TEST_EXPECT_EQ((u64)sts[1].uid,  (u64)0x1111u, "uid mapped");
        TEST_EXPECT_EQ((u64)sts[1].gid,  (u64)0x2222u, "gid mapped");
        TEST_EXPECT_EQ(sts[1].size, (u64)0x201, "size mapped (0x200+1)");
        walkqid_free(w);
        spoor_clunk(nc);   // clunks the bound fid
    }

    // BIND form, PARTIAL walk: the responder answers one short; the session
    // layer leaves new_fid unbound and nc must be UNTOUCHED (still sharing
    // the root's priv via the shallow clone).
    {
        g_dev9p_wga_partial = true;
        struct Spoor *nc = spoor_clone(root);
        TEST_ASSERT(nc != NULL, "clone");
        void *aux_before = nc->aux;
        struct t_stat sts[2];
        struct Walkqid *w = dev9p.walk_attrs(root, nc, names, lens, 2, sts);
        TEST_ASSERT(w != NULL, "partial walk returns the prefix");
        TEST_EXPECT_EQ(w->nqid, 1, "one component walked");
        TEST_ASSERT(w->spoor == NULL, "partial binds NOTHING");
        TEST_ASSERT(nc->aux == aux_before, "nc untouched (shallow aux intact)");
        TEST_EXPECT_EQ((u64)sts[0].qid_path, (u64)0x20, "prefix attrs present");
        walkqid_free(w);
        nc->aux = NULL;      // detach the shared aux; never clunk it
        spoor_unref(nc);
        g_dev9p_wga_partial = false;
    }

    // QUERY form (nc == NULL -> newfid = P9_NOFID): attrs only; no fid was
    // consumed on either end (the wire op carries NOFID; nothing to clunk).
    {
        struct t_stat sts[2];
        struct Walkqid *w = dev9p.walk_attrs(root, NULL, names, lens, 2, sts);
        TEST_ASSERT(w != NULL, "query walk");
        TEST_EXPECT_EQ(w->nqid, 2, "query walked both");
        TEST_ASSERT(w->spoor == NULL, "query transitions NOTHING");
        TEST_EXPECT_EQ((u64)sts[1].uid, (u64)0x1111u, "query attrs mapped");
        walkqid_free(w);
    }

    teardown(root);
}
