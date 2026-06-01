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
#include <thylacine/errno.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_p9_attached_create_destroy(void);
void test_p9_attached_handshake_failure_returns_null(void);
void test_p9_attached_handshake_rlerror_ecode_overflow_clamped(void);
void test_p9_attached_root_spoor_walk_read(void);
void test_p9_attached_query_helpers(void);
void test_p9_attached_walked_outlives_root_no_uaf(void);

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

// Responder that surfaces Rlerror(ecode=0x80000000) for Tattach -- the
// server-controlled value for which `-(int)ecode` is signed-overflow UB
// (A-3c audit F1). map_error MUST bound the wire ecode before negating
// (-> -EIO), so it neither traps under -fsanitize=undefined nor wraps.
static int handshake_fail_overflow_responder(void *ctx, const u8 *req, size_t req_len,
                                               u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if (type == P9_TVERSION) {
        return attach_responder(ctx, req, req_len, resp, resp_cap);
    }
    if (type == P9_TATTACH) {
        size_t total = P9_HDR_LEN + 4;
        if (resp_cap < total) return -1;
        resp[0] = 11; resp[1] = 0; resp[2] = 0; resp[3] = 0;
        resp[4] = P9_RLERROR;
        resp[5] = (u8)(tag & 0xff); resp[6] = (u8)((tag >> 8) & 0xff);
        resp[7] = 0x00; resp[8] = 0x00; resp[9] = 0x00; resp[10] = 0x80;  // 0x80000000 LE
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
        uname, sizeof(uname), aname, sizeof(aname), 0, NULL);
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
    int aerr = 0;
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback),
        4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0, &aerr);
    TEST_ASSERT(a == NULL, "attached creation fails on Rattach Rlerror");
    // A-3c / M6: the responder's Tattach Rlerror(ecode=13, EACCES) must be
    // SURFACED, not collapsed -- this is exactly the per-user-stratumd
    // dataset-scope refusal the SYS_ATTACH_9P* handlers map to a userspace
    // EACCES via attach_err_to_ret. Pre-M6 the ecode was lost (bare NULL).
    TEST_EXPECT_EQ((s64)aerr, (s64)(-T_E_ACCES),
                   "handshake Rlerror ecode surfaces as -T_E_ACCES");
    // No leak — p9_attached_create cleaned up the half-built client +
    // buffers on the handshake failure. Verifiable indirectly:
    // subsequent attempts to allocate succeed.
    p9_loopback_destroy(&g_loopback);
}

void test_p9_attached_handshake_rlerror_ecode_overflow_clamped(void) {
    int rc = p9_loopback_init(&g_loopback, g_loopback_resp,
                                sizeof(g_loopback_resp),
                                handshake_fail_overflow_responder, NULL);
    TEST_EXPECT_EQ(rc, 0, "loopback init (overflow ecode)");

    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    int aerr = 0;
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback),
        4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0, &aerr);
    TEST_ASSERT(a == NULL, "attach fails on Rlerror");
    // A-3c audit F1: a server-controlled ecode of 0x80000000 makes
    // `-(int)ecode` signed-overflow UB -- it TRAPS under -fsanitize=undefined
    // (a kernel halt reachable by any Rlerror on any op) and wraps to INT_MIN
    // on the plain build. map_error MUST bound the wire ecode before negating,
    // collapsing out-of-range values to -EIO. Pre-fix this assertion fails
    // (INT_MIN) on the default build and traps under UBSan.
    TEST_EXPECT_EQ((s64)aerr, (s64)(-P9_E_IO),
                   "out-of-range Rlerror ecode clamps to -EIO (no overflow UB)");
    p9_loopback_destroy(&g_loopback);
}

void test_p9_attached_root_spoor_walk_read(void) {
    p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                       attach_responder, NULL);
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback), 4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0, NULL);
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
        uname, sizeof(uname), aname, sizeof(aname), 0, NULL);
    TEST_ASSERT(a != NULL, "attached");

    TEST_ASSERT(p9_attached_is_open(a),          "is_open after create");
    TEST_ASSERT(!p9_attached_is_open(NULL),      "is_open(NULL) = false");

    p9_attached_destroy(a);
    // After destroy, `a` is freed — we can't call is_open on it
    // safely. Sanity-check that NULL still returns false.
    TEST_ASSERT(!p9_attached_is_open(NULL),      "is_open(NULL) still false post-destroy");
    p9_loopback_destroy(&g_loopback);
}

// F1 regression (P5-stratumd-stub-bringup audit close): the syscall
// handler's name-scratch + NUL-termination invariant. Pre-fix, the
// scratch was exactly SYS_WALK_OPEN_NAME_MAX bytes and the NUL
// terminator was written conditionally — `if (name_len_raw <
// SYS_WALK_OPEN_NAME_MAX)`. When name_len_raw == SYS_WALK_OPEN_NAME_MAX
// (64), the scratch was non-NUL-terminated; dev9p_walk's
// `while (s[l] != '\0') l++;` walked past it into adjacent kernel stack
// memory until it found a 0 byte. The discovered length was then
// shipped over the wire as part of the Twalk's wname[0], leaking
// kernel-stack contents to the user-controlled transport.
//
// Post-fix the scratch is SYS_WALK_OPEN_NAME_MAX + 1 bytes and the
// terminator is unconditional. This test exercises the boundary case
// by handing dev9p.walk a name buffer of EXACTLY SYS_WALK_OPEN_NAME_MAX
// bytes (with the NUL at position MAX), and verifies the wire encoding
// reports the name length as exactly SYS_WALK_OPEN_NAME_MAX. A
// regressed handler that fails to NUL-terminate would cause dev9p_walk
// to over-scan; the responder catches the over-scan via the Twalk
// frame's wname[0]_len field.
//
// The test exercises dev9p_walk's strlen behavior on a known-good
// (post-fix) buffer; the syscall handler's NUL-write is verified by
// code review (the unconditional `name_scratch[name_len_raw] = '\0';`
// is now structurally always-on). Together they pin the F1 invariant.

// File-scope captures for the F1 responder.
static u32 g_f1_captured_name_len;
static int g_f1_walk_seen;
static u8  g_f1_loopback_resp[4096];
static struct p9_loopback g_f1_loopback;

static int f1_64char_responder(void *ctx, const u8 *req, size_t req_len,
                                  u8 *resp, size_t resp_cap) {
    (void)ctx;
    if (req_len < P9_HDR_LEN) return -1;
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(req, req_len, &size, &type, &tag) < 0) return -1;
    if (type == P9_TVERSION || type == P9_TATTACH || type == P9_TCLUNK) {
        return attach_responder(ctx, req, req_len, resp, resp_cap);
    }
    if (type == P9_TWALK) {
        // Twalk layout: hdr(7) + fid(4) + newfid(4) + nwname(2) +
        // (nwname times: namelen(2) + name[namelen]).
        // Offsets:
        //   [7..10]  fid
        //   [11..14] newfid
        //   [15..16] nwname (component count)
        //   [17..18] wname[0]_len
        //   [19..]   wname[0] bytes
        // We capture wname[0]_len at byte 17-18.
        if (req_len < P9_HDR_LEN + 4 + 4 + 2 + 2) return -1;
        u16 namelen = (u16)req[17] | ((u16)req[18] << 8);
        g_f1_captured_name_len = (u32)namelen;
        g_f1_walk_seen = 1;
        // Synthesize a 1-qid Rwalk so the test progresses.
        return attach_responder(ctx, req, req_len, resp, resp_cap);
    }
    return attach_responder(ctx, req, req_len, resp, resp_cap);
}

void test_sys_walk_open_max_length_name_nul_terminated(void);

void test_sys_walk_open_max_length_name_nul_terminated(void) {
    g_f1_captured_name_len = 0;
    g_f1_walk_seen = 0;
    p9_loopback_init(&g_f1_loopback, g_f1_loopback_resp,
                       sizeof(g_f1_loopback_resp),
                       f1_64char_responder, NULL);
    const u8 uname[] = {'r'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_f1_loopback), 4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0, NULL);
    TEST_ASSERT(a != NULL, "attached created");

    struct Spoor *root = p9_attached_root_spoor(a);
    TEST_ASSERT(root != NULL, "root produced");

    // Construct a 64-byte name buffer with NUL at position 64 (mirroring
    // the post-fix handler's scratch layout).
    char name64[65];
    for (int i = 0; i < 64; i++) name64[i] = 'A';
    name64[64] = '\0';
    const char *names[1] = { name64 };

    struct Spoor *walked = spoor_clone(root);
    TEST_ASSERT(walked != NULL, "spoor_clone");
    struct Walkqid *w = dev9p.walk(root, walked, names, 1);
    TEST_ASSERT(w != NULL, "walk succeeded with 64-char name");
    TEST_ASSERT(g_f1_walk_seen,
                "responder observed the Twalk");
    TEST_EXPECT_EQ((u64)g_f1_captured_name_len, (u64)64,
        "wname[0]_len on the wire MUST be exactly 64 — any over-scan from a "
        "non-NUL-terminated scratch would report a larger length here");
    walkqid_free(w);

    spoor_clunk(walked);
    spoor_clunk(root);
    p9_attached_destroy(a);
    p9_loopback_destroy(&g_f1_loopback);
}

// F2 regression (P5-stratumd-stub-bringup audit close; closes R15 F236).
//
// Pre-fix: closing the SYS_ATTACH_9P root Spoor ran p9_attached_destroy
// immediately, freeing the p9_client. Walked Spoors derived from the
// root retained a dangling client pointer; their subsequent close did
// p9_client_clunk(stale_client, fid) — UAF on c->magic at best, on
// c->transport/c->session in the SLUB-slot-recycled case.
//
// The exit-time reproduction is via `proc_free` calling handle_table_free
// which iterates handles in ASCENDING index order. attach_fd is allocated
// FIRST → has the smaller index → closes BEFORE walked fds. Walked fds
// then close against a freed client.
//
// Post-fix: p9_attached is refcounted; root + every walked dev9p_priv hold
// one ref. The LAST unref runs the destroy. Walked closes after root
// close still observe a live client + transport.
//
// This test simulates the syscall-path attached_owner stamp (which is what
// sys_attach_9p_handler does in production) and exercises the buggy close
// order. Pre-fix would UAF; post-fix the walked close cleanly clunks via
// the still-alive client.
void test_p9_attached_walked_outlives_root_no_uaf(void) {
    p9_loopback_init(&g_loopback, g_loopback_resp, sizeof(g_loopback_resp),
                       attach_responder, NULL);
    const u8 uname[] = {'r','o','o','t'};
    const u8 aname[] = {'/'};
    struct p9_attached *a = p9_attached_create(
        p9_loopback_ops_for(&g_loopback), 4096, 0, 8192,
        uname, sizeof(uname), aname, sizeof(aname), 0, NULL);
    TEST_ASSERT(a != NULL, "attached created");

    struct Spoor *root = p9_attached_root_spoor(a);
    TEST_ASSERT(root != NULL, "root Spoor produced");

    // Simulate sys_attach_9p_handler's attached_owner stamp on the root.
    // The handler stamps + bumps the ref so the root contributes one
    // p9_attached_ref. (In production this happens in syscall.c right
    // after p9_attached_root_spoor returns; here we do it directly so
    // the test exercises the F2 close-order discipline without needing
    // the full syscall plumb.)
    struct dev9p_priv *root_priv = (struct dev9p_priv *)root->aux;
    TEST_ASSERT(root_priv != NULL && root_priv->magic == DEV9P_PRIV_MAGIC,
                "root priv has expected magic");
    root_priv->attached_owner = a;
    p9_attached_ref(a);   // root's hold; matches handler's bump

    // Walk to a derived Spoor. dev9p_walk's priv_alloc inherits the
    // root's attached_owner and bumps the ref to 3 (1 construction +
    // root's hold + walked's hold).
    struct Spoor *walked = spoor_clone(root);
    TEST_ASSERT(walked != NULL, "spoor_clone(root) succeeded");
    const char *name = "file";
    struct Walkqid *w = dev9p.walk(root, walked, &name, 1);
    TEST_ASSERT(w != NULL, "walk succeeded");
    TEST_ASSERT(w->spoor == walked, "walk reuses nc per Dev contract");
    walkqid_free(w);
    struct dev9p_priv *walked_priv = (struct dev9p_priv *)walked->aux;
    TEST_ASSERT(walked_priv != NULL &&
                walked_priv->attached_owner == a,
                "walked priv inherits attached_owner");

    // Drop the construction ref — root + walked hold the only refs now.
    p9_attached_unref(a);

    // Close the root FIRST (the F236 trigger ordering — what
    // handle_table_free does on Proc exit, since attach_fd has the
    // smaller index). Pre-fix this ran p9_attached_destroy immediately,
    // freeing the client; post-fix this just drops the root's ref.
    spoor_clunk(root);

    // Now close the walked. Pre-fix this UAF'd via p9_client_clunk on
    // a freed client. Post-fix the client is still alive (walked still
    // held one ref) and the clunk reaches the loopback responder
    // cleanly. The final p9_attached_unref runs in dev9p_close →
    // attached_owner unref → ref drops to 0 → attached_destroy_inner.
    spoor_clunk(walked);
    // If we got here without an extinct / UAF crash, F2 is closed.
    // The attached is destroyed by walked's last unref; we don't call
    // p9_attached_destroy(a) ourselves.

    p9_loopback_destroy(&g_loopback);
}
