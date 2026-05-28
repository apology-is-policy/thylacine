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
#include <thylacine/spoor.h>
#include <thylacine/types.h>

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

// File-scope buffers — client is ~12 KiB, won't fit on the 16 KiB test
// thread stack alongside a few smaller locals.
static struct p9_client g_client;
static struct p9_loopback g_loopback;
static u8 g_recv_buf[4096];
static u8 g_loopback_resp[4096];

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
