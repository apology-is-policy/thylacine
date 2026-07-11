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
void test_dev9p_page_cache_serve_and_gate(void);
void test_dev9p_cached_open(void);
void test_dev9p_cached_open_fallbacks(void);
void test_dev9p_cached_open_loose(void);

// File-scope buffers — client is ~80 KiB (the embedded Larder attr+dentry+page
// metadata), won't fit on the 16 KiB test thread stack alongside a few locals.
static struct p9_client g_client;
static struct p9_loopback g_loopback;
// Sized to the fixture's negotiated msize (8192): a 9P endpoint's recv/resp
// buffers must hold a full msize frame (the task-#44 big-file Rread is the
// first reply that actually needs it -- the "hello"-era 4096 was under-msize).
static u8 g_recv_buf[8192];
static u8 g_loopback_resp[8192];

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
static u64 g_tread_seen;        // Tread RPC counter (task-#44 wire-op assertions)
static u64 g_tread_file_size;   // != 0: the loopback serves a pattern file this big

// POUNCE: when set, the Twalkgetattr responder answers one component SHORT
// (a partial walk) -- the session layer must then leave newfid unbound and
// dev9p_walk_attrs must leave nc untouched.
static bool g_dev9p_wga_partial;

// FID-LIFECYCLE cached-open: wire-op counters (which ops actually hit the
// wire -- the fidless open's whole point is Tlopen/Tclunk NEVER do) + shape
// overrides on the Rwalkgetattr body so tests can drive the leaf's size /
// qid.version / qid.type (the canonical body is size 0x200+e / vers 0 / FILE).
static u32  g_wga_seen, g_lopen_seen, g_clunk_seen;
static bool g_wga_size_ov_on;
static u64  g_wga_size_ov;
static u32  g_wga_vers_ov;    // 0 = the canonical body (version 0)
static u8   g_wga_type_ov;    // 0 = the canonical P9_QTFILE

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
        g_clunk_seen++;
        // Empty body.
        if (resp_cap < P9_HDR_LEN) return -1;
        resp[0] = 7; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RCLUNK;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        return (int)P9_HDR_LEN;
    }
    if (type == P9_TLOPEN) {
        g_lopen_seen++;
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
        g_tread_seen++;
        // Big-file mode (task-#44 aligned-read tests): a pattern file of
        // g_tread_file_size bytes, honoring offset + count (the client already
        // clamps count to the msize payload, 8169 -- the SAME non-page-multiple
        // stride shape as production's 131049). byte(o) = pattern below.
        if (g_tread_file_size) {
            u64 off = le64_at(req + 11);
            u32 cnt = le32_at(req + 19);
            u64 rem = (off < g_tread_file_size) ? g_tread_file_size - off : 0;
            u32 n = (cnt < rem) ? cnt : (u32)rem;
            size_t btotal = P9_HDR_LEN + 4 + n;
            if (resp_cap < btotal) return -1;
            resp[0] = (u8)(btotal & 0xff); resp[1] = (u8)((btotal >> 8) & 0xff);
            resp[2] = (u8)((btotal >> 16) & 0xff); resp[3] = 0;
            resp[4] = P9_RREAD;
            resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
            resp[7] = (u8)(n & 0xff); resp[8] = (u8)((n >> 8) & 0xff);
            resp[9] = (u8)((n >> 16) & 0xff); resp[10] = (u8)((n >> 24) & 0xff);
            for (u32 i = 0; i < n; i++) {
                u64 o = off + i;
                resp[11 + i] = (u8)((o * 131u) ^ (o >> 8));
            }
            return (int)btotal;
        }
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
        g_wga_seen++;
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
            resp[o] = g_wga_type_ov ? g_wga_type_ov : P9_QTFILE;   // qid.type
            { u32 v = g_wga_vers_ov;                         // qid.version
              resp[o + 1] = (u8)v;         resp[o + 2] = (u8)(v >> 8);
              resp[o + 3] = (u8)(v >> 16); resp[o + 4] = (u8)(v >> 24); }
            o += 1 + 4;
            resp[o] = (u8)(0x20 + e); o += 8;                // qid.path
            { u32 m = 0100644u;                              // mode
              resp[o] = (u8)m; resp[o + 1] = (u8)(m >> 8);
              resp[o + 2] = (u8)(m >> 16); resp[o + 3] = (u8)(m >> 24); o += 4; }
            resp[o] = 0x11; resp[o + 1] = 0x11; o += 4;      // uid = 0x1111
            resp[o] = 0x22; resp[o + 1] = 0x22; o += 4;      // gid = 0x2222
            resp[o] = 1; o += 8;                             // nlink
            o += 8;                                          // rdev
            if (g_wga_size_ov_on) {                          // size (overridden)
                u64 sz = g_wga_size_ov;
                for (int b = 0; b < 8; b++) resp[o + b] = (u8)(sz >> (8 * b));
                o += 8;
            } else {
                resp[o] = (u8)e; resp[o + 1] = 0x02; o += 8; // size = 0x200 + e
            }
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
    // FID-LIFECYCLE async-clunk: dev9p_close fires the Tclunk FIRE-AND-FORGET.
    // The fid unbinds at SEND (I-11), but the Rclunk is NOT drained here -- the
    // tag stays outstanding until a later op's elected reader pumps it (I-10, the
    // #845 ownerless drain). The single-slot loopback has no later op, so the
    // deferred reply is verified by driving the reader pump explicitly (exactly
    // what the next real op does).
    TEST_ASSERT(!p9_session_fid_bound(&g_client.session, 1),
                 "walk-allocated fid 1 is unbound at send (async-clunk)");
    size_t after_send = p9_session_inflight(&g_client.session);
    TEST_EXPECT_EQ((u64)after_send, (u64)(before + 1),
                    "async-clunk leaves the Tclunk outstanding (deferred, not synchronous)");
    (void)p9_client_reader_pump_once(&g_client);
    size_t after_drain = p9_session_inflight(&g_client.session);
    TEST_EXPECT_EQ((u64)after_drain, (u64)before,
                    "the ownerless Rclunk drains via the reader pump (tag freed)");

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

// L1e (L1f audit F1): the reused-ino hazard applies to the child's PAGES exactly
// as to its attr. A create at a freed+reused qid.path can carry a stale prior-
// occupant page whose cvers collides with the fresh file's qid.version -- a
// collision the Thylacine tree cannot rule out (it depends on Stratum's fresh-
// inode si_cvers). dev9p_create must invalidate the child's pages, mirroring the
// attr defense above. Pre-seed a stale page at 0x77 (the created file's qid);
// verify create drops it. FAILS pre-fix (create invalidated the child's attr +
// the parent, never the child's pages -- the only page invalidate was
// dev9p_write).
void test_dev9p_create_invalidates_reused_child_pages(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");
    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);
    TEST_ASSERT(w != NULL, "clone-walk");
    walkqid_free(w);

    // Pre-seed a stale page at qid.path 0x77, page 0, cvers 1 -- as if a prior
    // file at that reused ino had a page cached.
    static const u8 stale_pg[5] = { 'S', 'T', 'A', 'L', 'E' };
    larder_page_install(&g_client.larder, larder_gen_snapshot(&g_client.larder),
                        0x77, /*page_index=*/0, /*cvers=*/1, stale_pg, 5);
    u8 probe[16]; u64 s0 = 0;
    TEST_EXPECT_EQ((u64)larder_page_serve(&g_client.larder, 0x77, 0, 0, 16,
                                          /*want_cvers=*/1, probe, &s0),
                   (u64)5, "stale child page present pre-create");

    struct Spoor *opened = dev9p.create(nc, "newfile", 1 /*OWRITE*/, 0644u, 1000u);
    TEST_ASSERT(opened == nc, "create returns the Spoor");
    TEST_EXPECT_EQ((u64)nc->qid.path, (u64)0x77, "qid from Rlcreate");
    TEST_EXPECT_EQ((u64)larder_page_serve(&g_client.larder, 0x77, 0, 0, 16, 1,
                                          probe, &s0),
                   (u64)0, "create invalidates the reused child's stale page");

    larder_destroy(&g_client.larder);
    spoor_clunk(nc);
    teardown(root);
}

// L1d: dev9p_create must invalidate the PARENT's cached dentries (own-write) so a
// stale NEGATIVE entry for the created name cannot serve ENOENT for the new file.
// The parent's si_cvers does NOT bump on a create (Stratum stores dirents in a
// separate index -- verified src/fs/fs.c), so own-write invalidation is the SOLE
// coherence mechanism (no parent-cvers gate). Non-vacuous: fails if the
// dev9p_create -> larder_dentry_invalidate_parent hook is missing.
void test_dev9p_create_invalidates_negative_dentry(void) {
    struct Spoor *root = make_open_client_and_root();
    TEST_ASSERT(root != NULL, "root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone target");
    struct Walkqid *w = dev9p.walk(root, nc, NULL, 0);
    TEST_ASSERT(w != NULL, "clone-walk");
    walkqid_free(w);

    // dev9p_create captures parent_path = c->qid.path pre-transition; nc inherited
    // root's qid via the 0-element clone-walk, so this is the parent dir.
    u64 parent = nc->qid.path;
    larder_dentry_install(&g_client.larder, larder_gen_snapshot(&g_client.larder),
                          parent, "newfile", 7, 0, /*negative=*/true);
    const char *nm[] = { "newfile" };
    size_t      ln[] = { 7 };
    struct t_stat sts[2];
    int nres; bool miss;
    TEST_ASSERT(larder_walk_serve(&g_client.larder, parent, nm, ln, 1, sts, &nres, &miss) &&
                miss, "negative dentry serves the miss pre-create");

    struct Spoor *opened = dev9p.create(nc, "newfile", 1 /*OWRITE*/, 0644u, 1000u);
    TEST_ASSERT(opened == nc, "create returns the Spoor");
    TEST_ASSERT(!larder_walk_serve(&g_client.larder, parent, nm, ln, 1, sts, &nres, &miss),
                "create invalidates the stale NEGATIVE dentry (no ENOENT for the new file)");

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

    // These sub-tests flip the wire result for the SAME path (dirA/fileB) by
    // toggling g_dev9p_wga_partial -- a non-physical change in the single-writer
    // model (a name cannot appear/vanish without a create/unlink that would
    // invalidate the Larder). Reset the L1d dentry/attr cache before each so this
    // WIRE-mechanics test exercises the wire fresh, not a stale served entry.
    struct larder *lard = &dev9p_priv_of(root)->client->larder;

    // BIND form, FULL walk: nc transitions (own fid, leaf qid), and the
    // fused attrs map exactly as dev9p_stat_native maps an Rgetattr
    // (mode/uid/gid under the valid mask; size; qid).
    {
        larder_init(lard);
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
        spoor_clunk(nc);   // clunks the bound fid (FID-LIFECYCLE async-clunk)
        // async-clunk defers the Rclunk; drain it before the next sub-test's op
        // so the single-slot loopback is not left holding a stale reply (the real
        // system drains it via the next op's reader).
        (void)p9_client_reader_pump_once(&g_client);
    }

    // BIND form, PARTIAL walk: the responder answers one short; the session
    // layer leaves new_fid unbound and nc must be UNTOUCHED (still sharing
    // the root's priv via the shallow clone).
    {
        larder_init(lard);
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
        larder_init(lard);
        g_dev9p_wga_partial = false;
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

// L1e integration: a read on a CACHEABLE client populates the page cache; a
// re-read of the same offset is served from the cache with NO second Tread. The
// cacheability GATE: on a non-cacheable client (the default -- no walk_attrs has
// proven POUNCE support), a read is never page-cached, so the re-read RPCs (a
// netd /net stream is never served stale). The sentinel-offset trick reuses the
// loopback's Tread-offset capture as a "was a Tread sent?" probe: set it to a
// sentinel before the re-read; if the cache served, no Tread fires and it stays.
void test_dev9p_page_cache_serve_and_gate(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);   // per-component: leaves cacheable false
    walkqid_free(w);
    dev9p.open(nc, 0);
    u8 buf[64];

    // --- Gate OFF (cacheable=false, the default): no caching -> the re-read RPCs. ---
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    long g1 = dev9p.read(nc, buf, 64, 0);
    TEST_EXPECT_EQ((u64)g1, (u64)5, "gate-off first read");
    g_tread_req_offset = 0xDEAD;                            // sentinel
    long g2 = dev9p.read(nc, buf, 64, 0);
    TEST_EXPECT_EQ((u64)g2, (u64)5, "gate-off re-read");
    TEST_EXPECT_EQ(g_tread_req_offset, 0ull,
                   "gate-off re-read RPCs (non-cacheable client is not page-cached)");

    // --- Gate ON (cacheable=true): the re-read is served from the page cache. ---
    __atomic_store_n(&g_client.cacheable, true, __ATOMIC_RELAXED);
    long h1 = dev9p.read(nc, buf, 64, 0);                   // miss -> RPC + populate page 0
    TEST_EXPECT_EQ((u64)h1, (u64)5, "gate-on first read populates");
    TEST_ASSERT(buf[0] == 'h' && buf[4] == 'o', "populate payload correct");
    g_tread_req_offset = 0xDEAD;                            // sentinel
    long h2 = dev9p.read(nc, buf, 64, 0);                   // HIT -> served from cache
    TEST_EXPECT_EQ((u64)h2, (u64)5, "gate-on re-read serves the cached bytes");
    TEST_ASSERT(buf[0] == 'h' && buf[4] == 'o', "served payload correct");
    TEST_EXPECT_EQ(g_tread_req_offset, 0xDEADull,
                   "gate-on re-read served from the page cache (NO second Tread)");
    TEST_ASSERT(g_client.larder.page_hits >= 1ull, "a page hit was counted");

    // An own-write invalidates the cached page: the next read RPCs again.
    dev9p.write(nc, (const u8 *)"x", 1, 0);
    g_tread_req_offset = 0xDEAD;
    long h3 = dev9p.read(nc, buf, 64, 0);
    TEST_EXPECT_EQ((u64)h3, (u64)5, "post-write read");
    TEST_EXPECT_EQ(g_tread_req_offset, 0ull,
                   "own-write invalidated the page (post-write read RPCs)");

    larder_destroy(&g_client.larder);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);   // restore default for teardown
    spoor_clunk(nc);
    teardown(root);
}

// Task-#44 aligned wire read: the msize payload (8169 here; 131049 in
// production) is not a page multiple, so a big sequential stream's chunks each
// end in a PARTIAL page. Pre-fix, the partial-front page could never be
// re-populated (populate starts at aligned starts >= the read offset), so the
// hole persisted and EVERY re-read of the stream pv-missed at it and re-paid
// the wire for the whole tail. The fix wires a big unaligned read at the
// containing page's ALIGNED start (a legal short read): each chunk fully
// rewrites its predecessor's partial tail, so pass 2 serves from pages.
void test_dev9p_read_align_heals_partial(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);
    dev9p.open(nc, 0);
    enum { FSZ = 20000 };
    g_tread_file_size = FSZ;
    __atomic_store_n(&g_client.cacheable, true, __ATOMIC_RELAXED);

    static u8 buf[16384];
    static u8 pass1[FSZ], pass2[FSZ];

    // Pass 1 (first touch): stream to EOF. Chunk 1 wires at 0 and ends at 8169
    // (page 1 partial, valid_len 4073); chunk 2 MUST wire at the ALIGNED 4096
    // (healing page 1), not the caller's 8169.
    u64 off = 0; long n;
    u64 chunk2_wire = 0; int chunk_i = 0;
    while ((n = dev9p.read(nc, buf, (long)sizeof buf, (s64)off)) > 0) {
        TEST_ASSERT(off + (u64)n <= (u64)FSZ, "pass-1 bounds");
        for (long i = 0; i < n; i++) pass1[off + (u64)i] = buf[i];
        off += (u64)n;
        chunk_i++;
        if (chunk_i == 1) g_tread_req_offset = 0xEEEE;   // sentinel for chunk 2
        if (chunk_i == 2) chunk2_wire = g_tread_req_offset;
    }
    TEST_EXPECT_EQ(off, (u64)FSZ, "pass 1 read the whole file");
    TEST_EXPECT_EQ(chunk2_wire, 4096ull,
                   "chunk 2 wired at the ALIGNED offset (4096, not 8169)");
    bool ok = true;
    for (u64 o = 0; o < (u64)FSZ; o++)
        if (pass1[o] != (u8)((o * 131u) ^ (o >> 8))) { ok = false; break; }
    TEST_ASSERT(ok, "pass-1 bytes match the pattern (the shift is byte-exact)");

    // Pass 2 (re-stream): every page serves from the cache; the ONLY wire op
    // is the tail EOF probe (no fresh attr installed in this fixture). Pre-fix
    // the holes persisted and the stream tail re-wired every pass.
    u64 wires0 = g_tread_seen;
    off = 0;
    while ((n = dev9p.read(nc, buf, (long)sizeof buf, (s64)off)) > 0) {
        for (long i = 0; i < n; i++) pass2[off + (u64)i] = buf[i];
        off += (u64)n;
    }
    TEST_EXPECT_EQ(off, (u64)FSZ, "pass 2 read the whole file");
    TEST_EXPECT_EQ(g_tread_seen - wires0, 1ull,
                   "pass 2 wired ONLY the EOF probe (holes healed; pages serve)");
    ok = true;
    for (u64 o = 0; o < (u64)FSZ; o++)
        if (pass2[o] != pass1[o]) { ok = false; break; }
    TEST_ASSERT(ok, "pass-2 bytes identical to pass 1");

    larder_destroy(&g_client.larder);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    g_tread_file_size = 0;
    spoor_clunk(nc);
    teardown(root);
}

// Task-#44 attr-served EOF: a FRESH cached attr (cvers == the fid's open-time
// qid.vers -- the page-serve freshness rule) answers the sequential reader's
// final read-returns-0 probe RPC-free; absent/stale attrs stay conservative
// (the probe wires). Own-write invalidation restores the wire probe.
void test_dev9p_read_eof_attr_served(void) {
    struct Spoor *root = make_open_client_and_root();
    struct Spoor *nc = spoor_clone(root);
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, nc, &name, 1);
    walkqid_free(w);
    dev9p.open(nc, 0);
    g_tread_file_size = 20000;
    __atomic_store_n(&g_client.cacheable, true, __ATOMIC_RELAXED);
    struct larder *l = &g_client.larder;
    u8 buf[64];

    // No attr cached: the EOF probe pays a wire RPC (conservative).
    u64 w0 = g_tread_seen;
    long r = dev9p.read(nc, buf, 64, 20000);
    TEST_EXPECT_EQ((u64)r, 0ull, "EOF read returns 0");
    TEST_EXPECT_EQ(g_tread_seen - w0, 1ull, "no fresh attr -> the probe wires");

    // FRESH attr: EOF serves RPC-free; in-range reads still return bytes.
    struct t_stat st = {0};
    st.size = 20000; st.qid_path = nc->qid.path; st.qid_vers = nc->qid.vers;
    larder_attr_install(l, larder_gen_snapshot(l), nc->qid.path, nc->qid.vers, &st);
    w0 = g_tread_seen;
    r = dev9p.read(nc, buf, 64, 20000);
    TEST_EXPECT_EQ((u64)r, 0ull, "EOF read returns 0 (attr-served)");
    TEST_EXPECT_EQ(g_tread_seen - w0, 0ull, "fresh attr -> ZERO wire ops");
    r = dev9p.read(nc, buf, 64, 19990);
    TEST_EXPECT_EQ((u64)r, 10ull, "in-range read still returns bytes");

    // An own-write invalidates the attr: the next EOF probe wires again.
    dev9p.write(nc, (const u8 *)"x", 1, 0);
    w0 = g_tread_seen;
    r = dev9p.read(nc, buf, 64, 20000);
    TEST_EXPECT_EQ((u64)r, 0ull, "post-write EOF read returns 0");
    TEST_EXPECT_EQ(g_tread_seen - w0, 1ull, "post-write EOF probe wires");

    larder_destroy(&g_client.larder);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    g_tread_file_size = 0;
    spoor_clunk(nc);
    teardown(root);
}

// =============================================================================
// FID-LIFECYCLE cached-open (docs/FID-LIFECYCLE-DESIGN.md section 3.3).
// =============================================================================

// Prime the Larder metadata for `name`: a bind-form walk_attrs latches
// cacheable + installs the dentry + attr from the wire (the canonical
// responder: qid.path 0x20, size 0x200 unless overridden), then close the
// bound Spoor + drain its deferred Rclunk so the single-slot loopback is
// clean for the next wire op.
static void co_prime(struct Spoor *root, const char *name, size_t len) {
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "co_prime clone");
    const char *names[1] = { name };
    size_t lens[1] = { len };
    struct t_stat sts[DEV_WALK_ATTRS_MAX];
    struct Walkqid *w = dev9p.walk_attrs(root, nc, names, lens, 1, sts);
    TEST_ASSERT(w != NULL && w->spoor == nc, "co_prime bind walk");
    walkqid_free(w);
    spoor_clunk(nc);
    (void)p9_client_reader_pump_once(&g_client);   // drain the async Rclunk
}

void test_dev9p_cached_open(void) {
    struct Spoor *root = make_open_client_and_root();
    struct larder *l = &g_client.larder;
    const char *names[1] = { "fileA" };
    size_t lens[1] = { 5 };
    struct t_stat sts[DEV_WALK_ATTRS_MAX];

    co_prime(root, "fileA", 5);

    // Pages absent -> the RPC-free hint misses; NO wire op is spent.
    g_wga_seen = 0; g_lopen_seen = 0; g_clunk_seen = 0;
    struct Spoor *co = dev9p.open_cached(root, (const char *const *)names,
                                         lens, 1, sts);
    TEST_ASSERT(co == NULL, "no pages cached -> hint miss -> NULL");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "a hint miss costs no wire op");

    // Install the covering page: content [0, 0x200) at cvers 0 (the canonical
    // responder's fresh version).
    static u8 data[0x200];
    for (u32 i = 0; i < sizeof(data); i++) data[i] = (u8)(i * 7 + 3);
    larder_page_install(l, larder_gen_snapshot(l), 0x20, 0, 0, data,
                        (u32)sizeof(data));

    u64 budget0 = dev9p_co_budget_used();

    // The fidless open: exactly ONE wire op (the forced-fresh query), no
    // Tlopen, the fresh records in sts, the budget charged.
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co != NULL, "cached-open mints the fidless Spoor");
    TEST_EXPECT_EQ((u64)g_wga_seen, 1ull, "exactly one wire op (the fresh query)");
    TEST_EXPECT_EQ((u64)g_lopen_seen, 0ull, "no Tlopen");
    TEST_ASSERT((co->flag & COPEN) != 0, "the Spoor is opened");
    TEST_EXPECT_EQ(co->qid.path, 0x20ull, "the leaf qid");
    struct dev9p_priv *cp = dev9p_priv_of(co);
    TEST_ASSERT(cp != NULL && cp->cached_open, "cached_open priv");
    TEST_EXPECT_EQ((u64)cp->fid, (u64)P9_NOFID, "fidless (P9_NOFID)");
    TEST_EXPECT_EQ(sts[0].qid_path, 0x20ull, "fresh sts filled for the post-scan");
    TEST_EXPECT_EQ(sts[0].size, 0x200ull, "fresh leaf size");
    TEST_EXPECT_EQ(dev9p_co_budget_used() - budget0, 0x200ull, "budget charged");

    // Reads serve the snapshot: full, sliced, EOF at + past size.
    u8 buf[0x260];
    long r = dev9p.read(co, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ((u64)r, 0x200ull, "read serves the full snapshot");
    bool bytes_ok = true;
    for (u32 i = 0; i < 0x200; i++)
        if (buf[i] != data[i]) { bytes_ok = false; break; }
    TEST_ASSERT(bytes_ok, "snapshot bytes verbatim");
    r = dev9p.read(co, buf, 50, 100);
    TEST_EXPECT_EQ((u64)r, 50ull, "sliced read");
    TEST_ASSERT(buf[0] == data[100] && buf[49] == data[149], "slice bytes");
    r = dev9p.read(co, buf, 64, 0x200);
    TEST_EXPECT_EQ((u64)r, 0ull, "read at size -> EOF");
    r = dev9p.read(co, buf, 64, 5000);
    TEST_EXPECT_EQ((u64)r, 0ull, "read past size -> EOF");

    // fstat serves the open-time stat; the fidless seams fail loud / no-op.
    struct t_stat st;
    TEST_EXPECT_EQ((u64)dev9p.stat_native(co, &st), 0ull, "fidless fstat serves");
    TEST_EXPECT_EQ(st.size, 0x200ull, "fstat size");
    TEST_EXPECT_EQ(st.qid_path, 0x20ull, "fstat qid");
    TEST_ASSERT(dev9p.wstat_native(co, T_WSTAT_MODE, 0644, 0, 0) == -1,
                "wstat on a fidless fd fails LOUD (the documented seam)");
    TEST_EXPECT_EQ((u64)dev9p.fsync(co, 0), 0ull, "fsync no-ops 0 (read-only fd)");
    TEST_ASSERT(dev9p.write(co, "x", 1, 0) < 0, "write rejected");
    TEST_ASSERT(dev9p.readdir(co, buf, 64, 0) < 0, "readdir rejected");
    TEST_ASSERT(dev9p.walk(co, NULL, names, 1) == NULL,
                "walk FROM a fidless Spoor rejected (NOFID guard)");
    TEST_ASSERT(dev9p.walk_attrs(co, NULL, names, lens, 1, sts) == NULL,
                "walk_attrs FROM a fidless Spoor rejected (NOFID guard)");

    // Close: wire-free (no Tclunk) + the budget released.
    g_clunk_seen = 0;
    spoor_clunk(co);
    TEST_EXPECT_EQ((u64)g_clunk_seen, 0ull, "close sends NO Tclunk");
    TEST_EXPECT_EQ(dev9p_co_budget_used(), budget0, "budget uncharged at close");

    larder_destroy(l);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    teardown(root);
}

// B1 per-attach loose mode (I-38 opt-in; docs/chase/B1-VOTE.md): a LOOSE
// client's cached-open serves a FULL hint hit with ZERO wire ops (the wga
// query skipped; the snapshot at the CACHED cvers); a hint miss still
// returns NULL without a wire op from open_cached (first touch takes the
// normal path); a STRICT client is byte-unchanged (the sibling test above
// pins its exactly-one-wire-op contract, which fails if loose leaks).
void test_dev9p_cached_open_loose(void) {
    struct Spoor *root = make_open_client_and_root();
    struct larder *l = &g_client.larder;
    const char *names[1] = { "fileA" };
    size_t lens[1] = { 5 };
    struct t_stat sts[DEV_WALK_ATTRS_MAX];

    co_prime(root, "fileA", 5);
    static u8 data[0x200];
    for (u32 i = 0; i < sizeof(data); i++) data[i] = (u8)(i * 3 + 1);
    larder_page_install(l, larder_gen_snapshot(l), 0x20, 0, 0, data,
                        (u32)sizeof(data));

    g_client.loose = true;
    u64 budget0 = dev9p_co_budget_used();

    // The loose fidless open: ZERO wire ops -- the B1 contract. The sts
    // post-scan records come from the caches (the same attrs the L1c base
    // X-check serves).
    g_wga_seen = 0; g_lopen_seen = 0;
    struct Spoor *co = dev9p.open_cached(root, (const char *const *)names,
                                         lens, 1, sts);
    TEST_ASSERT(co != NULL, "loose cached-open mints the fidless Spoor");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "ZERO wire ops on a full hint hit");
    TEST_EXPECT_EQ((u64)g_lopen_seen, 0ull, "no Tlopen");
    struct dev9p_priv *cp = dev9p_priv_of(co);
    TEST_ASSERT(cp != NULL && cp->cached_open, "cached_open priv");
    TEST_EXPECT_EQ((u64)cp->fid, (u64)P9_NOFID, "fidless (P9_NOFID)");
    TEST_EXPECT_EQ(sts[0].qid_path, 0x20ull, "cached sts filled for the post-scan");
    TEST_EXPECT_EQ(sts[0].size, 0x200ull, "cached leaf size");
    TEST_EXPECT_EQ(dev9p_co_budget_used() - budget0, 0x200ull, "budget charged");

    // Reads serve the snapshot taken at the CACHED cvers.
    u8 buf[0x200];
    long r = dev9p.read(co, buf, (long)sizeof(buf), 0);
    TEST_EXPECT_EQ((u64)r, 0x200ull, "read serves the full snapshot");
    bool bytes_ok = true;
    for (u32 i = 0; i < 0x200; i++)
        if (buf[i] != data[i]) { bytes_ok = false; break; }
    TEST_ASSERT(bytes_ok, "snapshot bytes verbatim");
    spoor_clunk(co);
    TEST_EXPECT_EQ(dev9p_co_budget_used(), budget0, "budget uncharged at close");

    // A hint MISS on a loose client: NULL and STILL zero wire ops from
    // open_cached (first touch belongs to the normal path -- the loose
    // mode never invents state it has not cached).
    const char *miss[1] = { "absent" };
    size_t mlen[1] = { 6 };
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)miss, mlen, 1, sts);
    TEST_ASSERT(co == NULL, "hint miss -> NULL on a loose client");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "a loose hint miss costs no wire op");

    // An own-write invalidate drops the coverage: the next loose
    // cached-open must MISS (fall back), not serve the stale snapshot.
    larder_page_invalidate(l, 0x20);
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co == NULL, "own-write invalidate -> loose hint miss");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "no wire op on the post-invalidate miss");

    g_client.loose = false;
    larder_destroy(l);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    teardown(root);
}

void test_dev9p_cached_open_fallbacks(void) {
    struct Spoor *root = make_open_client_and_root();
    struct larder *l = &g_client.larder;
    const char *names[1] = { "fileB" };
    size_t lens[1] = { 5 };
    struct t_stat sts[DEV_WALK_ATTRS_MAX];
    static u8 data[0x200];
    for (u32 i = 0; i < sizeof(data); i++) data[i] = (u8)(i ^ 0x5a);

    co_prime(root, "fileB", 5);

    // (a) Page cvers stale vs the CACHED attr -> the hint misses; no wire op.
    larder_page_install(l, larder_gen_snapshot(l), 0x20, 0, /*cvers=*/7, data,
                        (u32)sizeof(data));
    g_wga_seen = 0;
    struct Spoor *co = dev9p.open_cached(root, (const char *const *)names,
                                         lens, 1, sts);
    TEST_ASSERT(co == NULL, "stale page cvers (hint) -> NULL");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "no wire op on a hint miss");

    // (b) THE B2 REGRESSION: the hint passes (cached attr cvers 0 == page
    // cvers 0) but the SERVER's fresh version moved -> the FRESH gate must
    // fail the open. A buggy impl that trusted the locally-served attr
    // (B1/loose) would mint here and serve stale content.
    larder_page_install(l, larder_gen_snapshot(l), 0x20, 0, /*cvers=*/0, data,
                        (u32)sizeof(data));
    g_wga_vers_ov = 5;
    u64 b0 = dev9p_co_budget_used();
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co == NULL, "fresh-cvers mismatch -> fallback (B2, not B1)");
    TEST_EXPECT_EQ((u64)g_wga_seen, 1ull, "the query DID reach the wire");
    TEST_EXPECT_EQ(dev9p_co_budget_used(), b0, "budget balanced on the miss");
    g_wga_vers_ov = 0;

    // (c) Partial coverage: valid_len short of the required boundary (the
    // msize-clamp shape) -> the hint's coverage fails; no served hole.
    larder_page_install(l, larder_gen_snapshot(l), 0x20, 0, /*cvers=*/0, data,
                        300);
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co == NULL, "partial page -> coverage fails -> NULL");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "no wire op on a coverage miss");

    // (d) Oversize leaf: the size cap gates at the hint; no wire op.
    g_wga_size_ov_on = true;
    g_wga_size_ov = (u64)DEV9P_CO_MAX_SIZE + 1u;
    co_prime(root, "fileB", 5);           // re-prime the attr at the new size
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co == NULL, "over-cap size -> NULL");
    TEST_EXPECT_EQ((u64)g_wga_seen, 0ull, "no wire op on the size gate");

    // (e) A DIRECTORY leaf with size 0 would be "trivially covered" -- the
    // type gate MUST refuse it (a minted dir-as-file Spoor would break
    // readdir). Load-bearing, not defense.
    g_wga_size_ov = 0;                    // size 0: trivially covered
    g_wga_type_ov = P9_QTDIR;
    co_prime(root, "fileB", 5);
    g_wga_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co == NULL, "a size-0 DIRECTORY leaf is refused (type gate)");
    g_wga_type_ov = 0;

    // (f) The empty FILE: size 0, plain type -> the 1-RT fidless open works
    // (no pages needed; reads are EOF immediately).
    co_prime(root, "fileB", 5);           // attr: size 0, type FILE
    u64 b1 = dev9p_co_budget_used();
    g_wga_seen = 0; g_lopen_seen = 0; g_clunk_seen = 0;
    co = dev9p.open_cached(root, (const char *const *)names, lens, 1, sts);
    TEST_ASSERT(co != NULL, "empty-file cached-open mints");
    TEST_EXPECT_EQ((u64)g_wga_seen, 1ull, "one wire op");
    u8 buf[16];
    TEST_EXPECT_EQ((u64)dev9p.read(co, buf, 16, 0), 0ull, "empty file: EOF at 0");
    spoor_clunk(co);
    TEST_EXPECT_EQ((u64)g_lopen_seen + (u64)g_clunk_seen, 0ull,
                   "empty-file open+close fully wire-free beyond the query");
    TEST_EXPECT_EQ(dev9p_co_budget_used(), b1, "budget balanced (0-byte charge)");
    g_wga_size_ov_on = false;
    g_wga_size_ov = 0;

    larder_destroy(l);
    __atomic_store_n(&g_client.cacheable, false, __ATOMIC_RELAXED);
    teardown(root);
}
