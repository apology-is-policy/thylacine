// 9P byte-mode-SrvConn transport adapter tests (P6-pouch-stratumd-boot
// 16c).
//
// Tests:
//
//   9p_srvconn_transport.init_destroy
//     Init populates magic + cn + takes 1 srvconn_ref; is_open returns
//     true. Destroy clobbers magic; is_open returns false. (close drops
//     the ref; tested separately.)
//
//   9p_srvconn_transport.init_null_rejected
//     init with NULL adapter / NULL cn returns -1 + no ref taken.
//
//   9p_srvconn_transport.send_routes_to_c2s_ring
//     Adapter.send writes bytes through srvconn_client_send -> the
//     server side (srvconn_server_recv) drains them in order.
//
//   9p_srvconn_transport.recv_routes_from_s2c_ring
//     Bytes server-written via srvconn_server_send are returned by
//     adapter.recv. Uses a deadline so the test does not block
//     indefinitely on a missing producer (set 1s deadline; server
//     produces bytes before adapter.recv).
//
//   9p_srvconn_transport.close_drops_srvconn_ref
//     The adapter holds 1 ref; close drops it. Verified by the
//     srvconn_total_freed counter incrementing when the adapter is
//     the LAST holder (no userspace handle keeps the SrvConn alive).

#include "test.h"

#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

// Test-support registry wipe (non-static; defined in kernel/devsrv.c).
extern void srv_registry_reset(void);
extern u64 timer_now_ns(void);

// Forward decls for the registered test entries (suppress -Wmissing-
// prototypes; matches the rest of the kernel test corpus pattern).
void test_9p_srvconn_transport_init_destroy(void);
void test_9p_srvconn_transport_init_null_rejected(void);
void test_9p_srvconn_transport_send_routes_to_c2s_ring(void);
void test_9p_srvconn_transport_recv_routes_from_s2c_ring(void);
void test_9p_srvconn_transport_close_drops_srvconn_ref(void);
void test_9p_srvconn_transport_kernel_attached_skips_teardown_on_handle_close(void);
void test_9p_srvconn_transport_send_arms_fresh_deadline(void);

// =============================================================================
// Helpers (mirror test_srv_client.c's pattern).
// =============================================================================

static struct Proc *make_marked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_mark_may_post_service(p);
    return p;
}

static struct Proc *make_test_proc(void) {
    return proc_alloc();
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// Post a byte-mode service (create=post) + open one connection (open=connect)
// via the production /srv path, and install the client conn-endpoint Spoor as
// a KOBJ_SPOOR handle. Returns the (server, client, srv_handle, conn_handle,
// cn) tuple via out-params; cn is recovered from the conn Spoor via
// devsrv_conn_of. All test procs reset on cleanup -- test should call
// cleanup_byte_mode_pair on the way out. (stalk-3c retired the name-only
// SYS_POST_SERVICE_BYTE / SYS_SRV_CONNECT; this drives devsrv_post_listener +
// devsrv_open_connect, the same machinery stalk's SYS_WALK_CREATE / SYS_OPEN
// reach.)
static struct SrvConn *open_byte_mode_pair(struct Proc **out_server,
                                            struct Proc **out_client,
                                            int *out_svc_h, int *out_conn_h) {
    struct Proc *server = make_marked_test_proc();
    if (!server) return NULL;

    // create=post (byte mode) on a transient boot /srv root.
    struct Spoor *proot = devsrv_attach_registry(srv_boot_registry());
    if (!proot) { drop_test_proc(server); return NULL; }
    int svc_h = devsrv_post_listener(server, proot, "btest", 5, SRV_MODE_BYTE);
    spoor_clunk(proot);
    if (svc_h < 0) { drop_test_proc(server); return NULL; }

    struct Proc *client = make_test_proc();
    if (!client) { drop_test_proc(server); return NULL; }

    // open=connect: walk /srv/btest -> a service-ref Spoor, then
    // devsrv_open_connect -> a CSRVCLIENT byte-conn endpoint Spoor.
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) { drop_test_proc(server); drop_test_proc(client); return NULL; }
    struct Spoor *sref = spoor_clone(root);
    if (!sref) {
        spoor_clunk(root);
        drop_test_proc(server); drop_test_proc(client); return NULL;
    }
    const char *names[1] = { "btest" };
    struct Walkqid *w = devsrv.walk(root, sref, names, 1);
    if (!w) {
        spoor_clunk(sref); spoor_clunk(root);
        drop_test_proc(server); drop_test_proc(client); return NULL;
    }
    walkqid_free(w);
    struct Spoor *cs = devsrv_open_connect(client, sref, /*omode ORDWR*/ 2);
    spoor_clunk(sref);                 // the spent quarry (open-returns-new)
    spoor_clunk(root);
    if (!cs) { drop_test_proc(server); drop_test_proc(client); return NULL; }

    int conn_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    if (conn_h < 0) {
        spoor_clunk(cs);
        srv_registry_reset();
        drop_test_proc(server);
        drop_test_proc(client);
        return NULL;
    }
    struct SrvConn *cn = devsrv_conn_of(cs);

    *out_server = server;
    *out_client = client;
    *out_svc_h  = svc_h;
    *out_conn_h = conn_h;
    return cn;
}

static void cleanup_byte_mode_pair(struct Proc *server, struct Proc *client,
                                    int conn_h) {
    (void)conn_h;
    // Reset the registry: tombstones the service + drains the backlog;
    // pending SrvConns get the EOF treatment. handle_close on the
    // client's conn_h drops its hold; if the adapter test already
    // closed the adapter and dropped its hold, the SrvConn frees here.
    if (conn_h >= 0) handle_close(client, (hidx_t)conn_h);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// =============================================================================
// 9p_srvconn_transport.init_destroy
// =============================================================================

void test_9p_srvconn_transport_init_destroy(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    // Pre-state: open=connect minted the SrvConn and handed one ref to the
    // client conn-endpoint Spoor (the KOBJ_SPOOR conn_h) and one to the
    // accept backlog -- cn->ref == 2 here. init below takes a third.

    struct p9_srvconn_transport st;
    int rc = p9_srvconn_transport_init(&st, cn);
    TEST_EXPECT_EQ(rc, 0, "init succeeds");
    TEST_EXPECT_EQ((int)p9_srvconn_transport_is_open(&st), 1,
        "adapter is_open after init");

    p9_srvconn_transport_destroy(&st);
    TEST_EXPECT_EQ((int)p9_srvconn_transport_is_open(&st), 0,
        "adapter is_open == false after destroy");

    // Destroy does NOT drop the srvconn_ref the init took; the close
    // op does that. For this test we manually drop the leaked ref so
    // the cleanup path doesn't extinct on a ref leak when the client's
    // KObj_Srv drops its hold. (In production, the close vtable op
    // runs before destroy -- p9_attached_destroy calls
    // transport_ops.close.)
    srvconn_unref(cn);

    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.init_null_rejected
// =============================================================================

void test_9p_srvconn_transport_init_null_rejected(void) {
    struct p9_srvconn_transport st = { 0 };
    TEST_EXPECT_EQ(p9_srvconn_transport_init(NULL, NULL), -1,
        "init(NULL, NULL) returns -1");

    // With a non-NULL adapter slot but NULL cn -> -1, no state mutation.
    int rc = p9_srvconn_transport_init(&st, NULL);
    TEST_EXPECT_EQ(rc, -1, "init(&st, NULL) returns -1");
    TEST_EXPECT_EQ((int)st.magic, 0, "magic stays zero on rejected init");
    TEST_EXPECT_EQ((long)(unsigned long)st.cn, 0L,
        "cn stays NULL on rejected init");
}

// =============================================================================
// 9p_srvconn_transport.send_routes_to_c2s_ring
// =============================================================================

void test_9p_srvconn_transport_send_routes_to_c2s_ring(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");

    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

    static const u8 payload[] = { 0xCA, 0xFE, 0xBA, 0xBE,
                                   0xDE, 0xAD, 0xBE, 0xEF };
    int n = ops.send(ops.ctx, payload, sizeof(payload));
    TEST_EXPECT_EQ(n, (int)sizeof(payload),
        "adapter.send writes the full buffer");

    // Server-side drain via srvconn_server_recv (non-blocking).
    u8 rxbuf[sizeof(payload)];
    long got = srvconn_server_recv(cn, rxbuf, (long)sizeof(rxbuf));
    TEST_EXPECT_EQ((int)got, (int)sizeof(payload),
        "server side reads the same number of bytes");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_EXPECT_EQ((int)rxbuf[i], (int)payload[i],
            "c2s byte preserved");
    }

    (void)ops.close(ops.ctx);   // drops the adapter's srvconn_ref
    p9_srvconn_transport_destroy(&st);
    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.large_frame_roundtrip
//
// A-1b regression: corvus's first LARGE 9P write frame (>~128 B) over this
// byte-mode SrvConn fails at runtime while tiny frames work. Drives a 2048-byte
// payload through adapter.send -> c2s -> srvconn_server_recv_blocking (the path
// stratumd's serve loop uses), AT A WRAPPED RING POSITION (warm-up cycles
// advance head/tail near SRVCONN_RING_CAP so the big frame straddles the
// wraparound). Byte integrity + counts checked piecewise (header-then-body,
// like read_full).
// =============================================================================

void test_9p_srvconn_transport_large_frame_roundtrip(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");
    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

    static u8 big[2048];
    for (int i = 0; i < 2048; i++) big[i] = (u8)((i * 7 + 3) & 0xff);

    // Warm-up: cycle 400-byte frames to advance the ring head/tail near the
    // SRVCONN_RING_CAP (8192) boundary so the subsequent large frame wraps.
    static u8 warm[512];
    for (int cyc = 0; cyc < 20; cyc++) {
        int sn = ops.send(ops.ctx, big, 400);
        TEST_EXPECT_EQ(sn, 400, "warmup send full");
        long gn = srvconn_server_recv_blocking(cn, warm, 400);
        TEST_EXPECT_EQ((int)gn, 400, "warmup recv full");
    }

    // The large frame -- likely straddling the wraparound now.
    int sn = ops.send(ops.ctx, big, 2048);
    TEST_EXPECT_EQ(sn, 2048, "large adapter.send writes full 2048");

    // Drain server-side in pieces (4-byte header sim, then the rest) exactly
    // as stratumd's read_full would, accumulating until 2048.
    static u8 rx[2048];
    long off = 0;
    while (off < 2048) {
        long want = (off == 0) ? 4 : (2048 - off);
        long g = srvconn_server_recv_blocking(cn, rx + off, want);
        TEST_ASSERT(g > 0, "server recv piece > 0");
        off += g;
    }
    TEST_EXPECT_EQ((int)off, 2048, "server reassembles full 2048");
    int mism = 0;
    for (int i = 0; i < 2048; i++) {
        if (rx[i] != (u8)((i * 7 + 3) & 0xff)) { mism = i + 1; break; }
    }
    TEST_EXPECT_EQ(mism, 0, "every byte preserved across the wraparound");

    (void)ops.close(ops.ctx);
    p9_srvconn_transport_destroy(&st);
    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.send_arms_fresh_deadline
//
// Regression for the per-op deadline arm (the A-1b fsync-starvation fix):
// srvconn_transport_send MUST arm a fresh OP_DEADLINE window at each send, so a
// sequence of dev9p ops never SHARES one window (a slow op consuming the window
// would otherwise starve its successor into an immediate lapsed-deadline recv
// timeout). Pre-fix the send did not touch client_deadline_ns, so a near-lapsed
// value left by a prior op survived; this test sets that precondition and
// asserts the send overwrites it with a fresh now + OP_DEADLINE.
// =============================================================================

void test_9p_srvconn_transport_send_arms_fresh_deadline(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");
    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

    // Precondition: a near-exhausted shared window left by a prior slow op.
    // Pre-fix (recv-only "arm if lapsed") the next op's send would leave this
    // stale value, starving the recv into an immediate TSLEEP_TIMEDOUT.
    u64 stale = timer_now_ns() + 1u;
    srvconn_set_client_deadline(cn, stale);

    u8 frame[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
    u64 t0 = timer_now_ns();
    int n = ops.send(ops.ctx, frame, sizeof(frame));
    TEST_EXPECT_EQ(n, (int)sizeof(frame), "send writes full buffer");
    TEST_ASSERT(cn->client_deadline_ns > stale,
                "send overwrites the stale near-lapsed deadline");
    TEST_ASSERT(cn->client_deadline_ns >= t0 + SRVCONN_OP_DEADLINE_NS,
                "send arms a fresh full OP_DEADLINE window");

    // A second op's send re-arms again -- never inherits the remainder.
    u64 t1 = timer_now_ns();
    int n2 = ops.send(ops.ctx, frame, sizeof(frame));
    TEST_EXPECT_EQ(n2, (int)sizeof(frame), "second send writes full buffer");
    TEST_ASSERT(cn->client_deadline_ns >= t1 + SRVCONN_OP_DEADLINE_NS,
                "second send re-arms a fresh window");

    (void)ops.close(ops.ctx);
    p9_srvconn_transport_destroy(&st);
    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.recv_routes_from_s2c_ring
// =============================================================================

void test_9p_srvconn_transport_recv_routes_from_s2c_ring(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");

    // Server pre-fills s2c via srvconn_server_send. The kernel client
    // adapter then drains via blocking recv -- since data is already
    // present, the recv returns immediately without hitting the
    // deadline.
    static const u8 reply[] = { 0x11, 0x22, 0x33, 0x44,
                                 0x55, 0x66, 0x77, 0x88 };
    long sent = srvconn_server_send(cn, reply, (long)sizeof(reply));
    TEST_EXPECT_EQ((int)sent, (int)sizeof(reply),
        "server-side send accepts the full buffer");

    // Pre-set the client deadline -- adapter.recv's underlying
    // srvconn_client_recv consults cn->client_deadline_ns. Without it
    // and with the bytes already in s2c, recv returns immediately;
    // setting a deadline is defense-in-depth so the test cannot hang
    // forever if the helper changes.
    srvconn_set_client_deadline(cn, 0);   // no deadline (data is present)

    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);
    u8 rxbuf[sizeof(reply)];
    int n = ops.recv(ops.ctx, rxbuf, sizeof(rxbuf));
    TEST_EXPECT_EQ(n, (int)sizeof(reply),
        "adapter.recv pulls the full buffer");
    for (size_t i = 0; i < sizeof(reply); i++) {
        TEST_EXPECT_EQ((int)rxbuf[i], (int)reply[i],
            "s2c byte preserved");
    }

    (void)ops.close(ops.ctx);
    p9_srvconn_transport_destroy(&st);
    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.close_drops_srvconn_ref
// =============================================================================

void test_9p_srvconn_transport_close_drops_srvconn_ref(void) {
    srv_registry_reset();

    u64 freed_before = srvconn_total_freed();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0,
        "init takes +1 srvconn_ref");

    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

    // A freshly-minted SrvConn from open=connect has THREE holders pre-test:
    //   (a) the client's KOBJ_SPOOR conn-endpoint handle slot,
    //   (b) the service's accept backlog (server hasn't called accept),
    //   (c) the adapter we just init'd.
    // To isolate "adapter is the last holder" we must drop (a) and (b)
    // before close + observe the freed counter.

    TEST_EXPECT_EQ(srvconn_total_freed(), freed_before,
        "no freed-counter advance pre-close (multiple holders)");

    // (a) Drop the client's KObj_Srv hold.
    handle_close(client, (hidx_t)conn_h);

    // (b) Reset the service registry: tombstones the service, drains
    // the accept backlog, releases the backlog-side ref. Now only the
    // adapter's ref remains.
    srv_registry_reset();

    TEST_EXPECT_EQ(srvconn_total_freed(), freed_before,
        "SrvConn alive after client handle + backlog refs dropped -- "
        "adapter's ref keeps it");

    // Now close drops the adapter's ref -- last ref -> SrvConn frees.
    int crc = ops.close(ops.ctx);
    TEST_EXPECT_EQ(crc, 0, "close returns 0");
    TEST_EXPECT_EQ(srvconn_total_freed(), freed_before + 1,
        "adapter.close was the last unref; SrvConn frees");

    // Idempotent re-close: the inner pointer is NULL now.
    int crc2 = ops.close(ops.ctx);
    TEST_EXPECT_EQ(crc2, 0, "second close is a no-op success");

    p9_srvconn_transport_destroy(&st);
    drop_test_proc(server);
    drop_test_proc(client);
}

// =============================================================================
// 9p_srvconn_transport.kernel_attached_skips_teardown_on_handle_close
//
// F-16c-attach-srv close: with srvconn_set_kernel_attached(cn), the
// userspace handle_close on the KOBJ_SRV slot must skip srvconn_teardown
// (so the c2s/s2c rings stay live for the kernel 9P client). The pre-fix
// behavior tore down the rings, breaking the first Twalk send after
// SYS_ATTACH_9P_SRV when joey closed its now-redundant client handle.
// =============================================================================

void test_9p_srvconn_transport_kernel_attached_skips_teardown_on_handle_close(void) {
    // Part 1: control (no kernel_attached). Default handle_close path
    // tears down both rings.
    {
        srv_registry_reset();
        struct Proc *server = NULL, *client = NULL;
        int svc_h = -1, conn_h = -1;
        struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
        TEST_ASSERT(cn != NULL, "control: open_byte_mode_pair");
        srvconn_ref(cn);   // probe-ref so we can read AFTER handle_close

        handle_close(client, (hidx_t)conn_h);

        u8 probe;
        long control_recv = srvconn_server_recv(cn, &probe, 1);
        TEST_EXPECT_EQ(control_recv, -1,
            "control: default close tears down rings (server_recv = -1 = EOF)");

        srvconn_unref(cn);
        srv_registry_reset();
        drop_test_proc(server);
        drop_test_proc(client);
    }

    // Part 2: kernel_attached=true. handle_close MUST NOT tear down --
    // the rings stay live for the kernel 9P client.
    {
        srv_registry_reset();
        struct Proc *server = NULL, *client = NULL;
        int svc_h = -1, conn_h = -1;
        struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
        TEST_ASSERT(cn != NULL, "attached: open_byte_mode_pair");
        srvconn_ref(cn);

        // Sanity: rings live pre-close.
        u8 probe;
        long pre_recv = srvconn_server_recv(cn, &probe, 1);
        TEST_EXPECT_EQ(pre_recv, 0,
            "attached: c2s ring empty-but-live pre-close (server_recv = 0)");

        srvconn_set_kernel_attached(cn);
        TEST_EXPECT_EQ(srvconn_is_kernel_attached(cn), true,
            "kernel_attached flag set");

        handle_close(client, (hidx_t)conn_h);

        long post_recv = srvconn_server_recv(cn, &probe, 1);
        TEST_EXPECT_EQ(post_recv, 0,
            "attached: handle_close skipped teardown (server_recv = 0 = no data, not -1 = EOF)");

        srvconn_unref(cn);
        srv_registry_reset();
        drop_test_proc(server);
        drop_test_proc(client);
    }

    // Part 3 (R1 F8 close): verify the adapter's transport.close DOES
    // migrate the teardown. With kernel_attached=true, handle_close
    // skipped teardown; the rings stayed live. The adapter's
    // transport.close hook is now the SOLE teardown trigger. This
    // catches a future regression where the adapter's close drops the
    // teardown call (which would otherwise leave a kernel-attached
    // SrvConn permanently live after the FS unmount).
    {
        srv_registry_reset();
        struct Proc *server = NULL, *client = NULL;
        int svc_h = -1, conn_h = -1;
        struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
        TEST_ASSERT(cn != NULL, "adapter-close: open_byte_mode_pair");
        srvconn_ref(cn);

        struct p9_srvconn_transport st;
        TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0,
            "adapter-close: init takes +1 srvconn_ref");
        struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

        srvconn_set_kernel_attached(cn);
        handle_close(client, (hidx_t)conn_h);   // skip teardown (gated)

        u8 probe;
        long mid_recv = srvconn_server_recv(cn, &probe, 1);
        TEST_EXPECT_EQ(mid_recv, 0,
            "adapter-close: rings live after handle_close (kernel_attached gate)");

        // Call the adapter's transport.close -- this is the migrated
        // teardown path. Post-call the rings MUST be EOF'd.
        int crc = ops.close(ops.ctx);
        TEST_EXPECT_EQ(crc, 0, "adapter-close: ops.close returns 0");

        long post_recv = srvconn_server_recv(cn, &probe, 1);
        TEST_EXPECT_EQ(post_recv, -1,
            "adapter-close: transport.close completed the migrated teardown "
            "(server_recv = -1 = EOF)");

        srvconn_unref(cn);   // drop the probe ref we took
        srv_registry_reset();
        drop_test_proc(server);
        drop_test_proc(client);
    }
}
