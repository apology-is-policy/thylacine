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

#include <thylacine/9p_client.h>
#include <thylacine/9p_session.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/9p_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/loom.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

// Test-support registry wipe (non-static; defined in kernel/devsrv.c).
extern void srv_registry_reset(void);
extern u64 timer_now_ns(void);

// The shared 9P2000.L canned-reply builder (non-static; defined in
// test_9p_client.c). The SrvConn-vehicle device-gone tests pre-stage their
// handshake replies through it -- one source of truth for the reply byte
// layouts, so a future wire change cannot silently desync this file.
int canonical_responder(void *ctx, const u8 *req, size_t req_len,
                        u8 *resp, size_t resp_cap);

// Forward decls for the registered test entries (suppress -Wmissing-
// prototypes; matches the rest of the kernel test corpus pattern).
void test_9p_srvconn_transport_init_destroy(void);
void test_9p_srvconn_transport_init_null_rejected(void);
void test_9p_srvconn_transport_send_routes_to_c2s_ring(void);
void test_9p_srvconn_transport_recv_routes_from_s2c_ring(void);
void test_9p_srvconn_transport_close_drops_srvconn_ref(void);
void test_9p_srvconn_transport_kernel_attached_skips_teardown_on_handle_close(void);
void test_9p_srvconn_transport_send_preserves_caller_deadline(void);
void test_9p_srvconn_transport_deadline_vtable_routes(void);
void test_9p_srvconn_transport_devgone_posts_nodev_cqe(void);
void test_9p_srvconn_transport_transport_err_posts_eio_cqe(void);

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

    // Warm-up: cycle 400-byte frames to advance the ring head/tail to within
    // ~1 KiB of the SRVCONN_RING_CAP boundary so the subsequent 2048-byte frame
    // straddles the wraparound. The cycle count is DERIVED from SRVCONN_RING_CAP
    // so the wrap is exercised regardless of the ring size (Weft-0 raised it
    // 8 KiB -> 64 KiB; the prior fixed count of 20 stopped reaching the boundary
    // and silently turned this into a non-wrapping test).
    static u8 warm[512];
    unsigned warm_cycles = (SRVCONN_RING_CAP - 1024) / 400;
    for (unsigned cyc = 0; cyc < warm_cycles; cyc++) {
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
// 9p_srvconn_transport.send_preserves_caller_deadline
//
// #841: srvconn_transport_send no longer arms a per-op deadline. The deadline
// is caller-set -- srvconn_attach_dev9p_root arms HANDSHAKE_DEADLINE for the
// serial handshake, then resets it to 0 ("block until reply / EOF / death") for
// steady-state ops. A per-op arm whose expiry abandons one in-flight op desyncs
// the pipelined client's shared byte stream (the stalk-3c bug ARCH 21.10
// restores 21 to fix), so the send MUST leave client_deadline_ns exactly as the
// caller set it. (Supersedes the A-1b fsync-starvation "arm at send" fix: the
// no-timeout steady state removes the shared-window starvation by construction.)
// =============================================================================

void test_9p_srvconn_transport_send_preserves_caller_deadline(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");
    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);

    u8 frame[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };

    // A caller-set deadline (the handshake case) survives the send UNCHANGED.
    u64 set = timer_now_ns() + SRVCONN_HANDSHAKE_DEADLINE_NS;
    srvconn_set_client_deadline(cn, set);
    int n = ops.send(ops.ctx, frame, sizeof(frame));
    TEST_EXPECT_EQ(n, (int)sizeof(frame), "send writes full buffer");
    TEST_ASSERT(cn->client_deadline_ns == set,
                "send leaves the caller-set deadline untouched");

    // The steady-state case: deadline 0 (no timeout) stays 0 across the send,
    // so the elected reader blocks until reply / EOF / death.
    srvconn_set_client_deadline(cn, 0);
    int n2 = ops.send(ops.ctx, frame, sizeof(frame));
    TEST_EXPECT_EQ(n2, (int)sizeof(frame), "second send writes full buffer");
    TEST_ASSERT(cn->client_deadline_ns == 0u,
                "send does not re-arm a deadline (steady-state blocks)");

    (void)ops.close(ops.ctx);
    p9_srvconn_transport_destroy(&st);
    cleanup_byte_mode_pair(server, client, conn_h);
}

// =============================================================================
// 9p_srvconn_transport.deadline_vtable_routes (Loom-4, LOOM.md §8.6)
//
// The NULL-permitted set_recv_deadline / recv_timed_out vtable ops route to
// srvconn_set_client_deadline / srvconn_client_timed_out. A fresh deadline
// clears the signal; a PAST deadline makes the very next recv on an empty s2c
// ring time out promptly (tsleep returns TSLEEP_TIMEDOUT once now >= deadline),
// and the timed-out signal surfaces back through the recv_timed_out shim -- the
// mechanism the SQPOLL idle pump reads to tell IDLE from EOF over a real
// SrvConn-backed client.
// =============================================================================

void test_9p_srvconn_transport_deadline_vtable_routes(void) {
    srv_registry_reset();

    struct Proc *server = NULL;
    struct Proc *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    TEST_EXPECT_EQ(p9_srvconn_transport_init(&st, cn), 0, "init");
    struct p9_transport_ops ops = p9_srvconn_transport_ops(&st);
    TEST_ASSERT(ops.set_recv_deadline != NULL, "set_recv_deadline wired");
    TEST_ASSERT(ops.recv_timed_out != NULL, "recv_timed_out wired");

    u8 rbuf[64];
    struct p9_transport t;
    TEST_EXPECT_EQ(p9_transport_init(&t, ops, rbuf, sizeof(rbuf)), 0, "transport init");

    // A fresh (future) deadline routes through + clears the signal.
    p9_transport_set_recv_deadline(&t, timer_now_ns() + 1000000000ull);
    TEST_ASSERT(!p9_transport_recv_timed_out(&t), "freshly armed -> not yet timed out");

    // A past deadline + empty s2c -> the recv times out promptly, and the signal
    // surfaces through the recv_timed_out shim.
    p9_transport_set_recv_deadline(&t, timer_now_ns());
    u8 buf[16];
    int n = t.ops.recv(t.ops.ctx, buf, sizeof(buf));
    TEST_EXPECT_EQ(n, -1, "recv on empty s2c past the deadline -> -1");
    TEST_ASSERT(p9_transport_recv_timed_out(&t),
                "recv_timed_out routes to srvconn_client_timed_out (true)");

    (void)ops.close(ops.ctx);
    p9_transport_destroy(&t);
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

// =============================================================================
// 9p_srvconn_transport.devgone_posts_nodev_cqe       (Menagerie 5e-3)
// 9p_srvconn_transport.transport_err_posts_eio_cqe
//
// Step-4 (#162-165) threaded the death REASON through the async (Loom) 9P
// completion path: a clean peer-gone EOF (recv 0) completes an in-flight async
// op with the device-gone -P9_E_NODEV CQE; a transport error (recv -1) keeps
// the generic -P9_E_IO. Until now that distinction was proven only over the
// single-slot loopback test transport, where force_eof SYNTHESIZES the recv-0.
// These two tests prove it END-TO-END over the PRODUCTION p9_srvconn_transport:
// a real srvconn_teardown (the path a DeviceRemoved drives) latches s2c EOF, so
// the kernel client's recv returns 0 -> -ENODEV reaches a real Loom consumer,
// while a real deadline-elapsed recv returns -1 -> -EIO. The load-bearing claim
// is that the production SrvConn ACTUALLY produces recv-0-on-teardown (not -1),
// so the device-gone reason reaches the CQE consumer over the wire a driver's
// Loom rides -- the gap the loopback's synthetic force_eof cannot close.
// =============================================================================

struct sc_async_op {
    struct p9_rpc rpc;          // MUST be first: on_complete casts rpc -> op
    struct Loom  *loom;
    u64           user_data;
    s32           last_result;
    bool          completed;
};
_Static_assert(__builtin_offsetof(struct sc_async_op, rpc) == 0,
               "rpc must be first for the on_complete container cast");

static struct p9_client    g_sc_client;
static u8                  g_sc_recv_buf[8192];
static struct sc_async_op  g_sc_async;

static void sc_async_on_complete(struct p9_rpc *rpc, int status,
                                 struct p9_dispatch_result *dr) {
    struct sc_async_op *op = (struct sc_async_op *)rpc;   // rpc is first
    (void)dr;
    op->last_result = (s32)status;
    op->completed   = true;
    (void)loom_post_cqe(op->loom, op->user_data, (s32)status, 0);
}

// Build thunk: a Tfsync on the fid passed via ctx. send_fsync permits the
// attach-bound root fid (unlike send_clunk), so the test needs no walk -- one
// fewer pre-staged reply.
static int sc_build_fsync(struct p9_session *s, u8 *out, size_t cap, void *ctx) {
    u32 fid = *(u32 *)ctx;
    return p9_session_send_fsync(s, out, cap, fid, /*datasync=*/0);
}

// Build the canonical reply for a request of `type` + `tag` and stage it into
// the SrvConn's s2c ring (server -> kernel client). A unit test has no live
// server thread to produce replies, so the handshake replies are pre-positioned
// here; do_recv is framed (size-prefixed) so each phase consumes exactly one.
static int sc_stage_reply(struct SrvConn *cn, u8 type, u16 tag) {
    u8 req[P9_HDR_LEN];
    req[0] = P9_HDR_LEN; req[1] = 0; req[2] = 0; req[3] = 0;   // size = header-only
    req[4] = type;
    req[5] = (u8)(tag & 0xff); req[6] = (u8)((tag >> 8) & 0xff);
    u8 resp[64];
    int rlen = canonical_responder(NULL, req, sizeof(req), resp, sizeof(resp));
    if (rlen <= 0) return -1;
    long n = srvconn_server_send(cn, resp, (long)rlen);
    return (n == (long)rlen) ? 0 : -1;
}

// Stand up an OPEN 9P client (g_sc_client) over a real byte-mode SrvConn: wrap
// the client end in the production adapter, pre-stage Rversion (NOTAG) then
// Rattach (tag 0 -- the first alloc_tag on a fresh session; Tversion is NOTAG +
// reserves no slot), and run the real handshake to OPEN (binding root fid 0).
// *out_st is the adapter storage the caller keeps live until cleanup; *out_ops
// is its vtable (for the close that drops the adapter's srvconn_ref).
static int sc_open_handshaked(struct SrvConn *cn,
                              struct p9_srvconn_transport *out_st,
                              struct p9_transport_ops *out_ops) {
    if (p9_srvconn_transport_init(out_st, cn) != 0) return -1;
    *out_ops = p9_srvconn_transport_ops(out_st);

    srvconn_set_client_deadline(cn, 0);   // present data -> recv returns at once
    if (sc_stage_reply(cn, P9_TVERSION, P9_NOTAG) != 0) return -1;
    if (sc_stage_reply(cn, P9_TATTACH, 0) != 0)         return -1;

    if (p9_client_init(&g_sc_client, /*root_fid=*/0, /*msize=*/8192, *out_ops,
                       g_sc_recv_buf, sizeof(g_sc_recv_buf)) != 0)
        return -1;
    const u8 uname[] = { 'r','o','o','t' };
    const u8 aname[] = { '/' };
    return p9_client_handshake(&g_sc_client, uname, sizeof(uname),
                               aname, sizeof(aname), 0);
}

void test_9p_srvconn_transport_devgone_posts_nodev_cqe(void) {
    srv_registry_reset();

    struct Proc *server = NULL, *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    struct p9_transport_ops ops;
    TEST_EXPECT_EQ(sc_open_handshaked(cn, &st, &ops), 0,
        "handshake -> OPEN over the real srvconn");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // In-flight async op over the real transport: a Tfsync on the attach-bound
    // root fid 0. No reply is staged, so it stays registered in c->inflight[]
    // until device-gone completes it.
    g_sc_async.loom        = l;
    g_sc_async.user_data   = 0x5E3D00DFEEDULL;
    g_sc_async.last_result = 0x7fffffff;
    g_sc_async.completed   = false;
    g_sc_async.rpc.on_complete = sc_async_on_complete;
    u32 fid = 0;
    int rc = p9_client_submit_async(&g_sc_client, &g_sc_async.rpc, sc_build_fsync, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async(fsync) over srvconn -- op in flight");
    TEST_ASSERT(!g_sc_async.completed, "not completed before device-gone");

    // DEVICE GONE: tear down the real SrvConn (the production path a
    // DeviceRemoved drives). This latches s2c EOF, so the kernel client's next
    // recv returns 0 -- a CLEAN peer-gone EOF, NOT a -1 transport error. THE
    // LOAD-BEARING DISTINCTION: a real srvconn_teardown yields recv-0, which the
    // reader classifies device-gone -> -ENODEV. The loopback peer-gone test only
    // SYNTHESIZES the recv-0 (force_eof); this proves the production transport
    // actually produces it over the wire a driver's Loom rides.
    srvconn_teardown(cn);

    int pumped = p9_client_reader_pump_once(&g_sc_client);
    TEST_EXPECT_EQ(pumped, -P9_E_IO, "pump returns DEAD (a control signal)");
    TEST_ASSERT(g_sc_async.completed, "async op completed on the device-gone EOF");
    TEST_EXPECT_EQ((u64)(s64)g_sc_async.last_result, (u64)(s64)(-P9_E_NODEV),
        "device-gone CQE = -ENODEV (NOT -EIO) over the real srvconn");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "exactly one CQE reached the consumer");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-P9_E_NODEV),
        "the consumer's CQE carries -ENODEV");
    TEST_EXPECT_EQ(cqes[0].user_data, 0x5E3D00DFEEDULL,
        "CQE user_data echoed to the consumer");

    p9_client_destroy(&g_sc_client);
    (void)ops.close(ops.ctx);            // teardown (idempotent) + drop adapter ref
    p9_srvconn_transport_destroy(&st);
    loom_unref(l);
    cleanup_byte_mode_pair(server, client, conn_h);
}

void test_9p_srvconn_transport_transport_err_posts_eio_cqe(void) {
    srv_registry_reset();

    struct Proc *server = NULL, *client = NULL;
    int svc_h = -1, conn_h = -1;
    struct SrvConn *cn = open_byte_mode_pair(&server, &client, &svc_h, &conn_h);
    TEST_ASSERT(cn != NULL, "open_byte_mode_pair");

    struct p9_srvconn_transport st;
    struct p9_transport_ops ops;
    TEST_EXPECT_EQ(sc_open_handshaked(cn, &st, &ops), 0,
        "handshake -> OPEN over the real srvconn");

    struct Loom *l = loom_create(8, 16);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    g_sc_async.loom        = l;
    g_sc_async.user_data   = 0x5E3E10C0DEULL;
    g_sc_async.last_result = 0x7fffffff;
    g_sc_async.completed   = false;
    g_sc_async.rpc.on_complete = sc_async_on_complete;
    u32 fid = 0;
    int rc = p9_client_submit_async(&g_sc_client, &g_sc_async.rpc, sc_build_fsync, &fid);
    TEST_EXPECT_EQ(rc, 0, "submit_async(fsync) over srvconn -- op in flight");

    // TRANSPORT ERROR (not device-gone): arm a PAST recv deadline. The next recv
    // on the empty -- but NOT torn -- s2c times out and returns -1 (an error),
    // NOT 0 (a clean EOF). The reader classifies this transport, not device-gone
    // -> the in-flight op gets the generic -EIO. The contrast proves the SrvConn
    // distinguishes the two reasons: the -ENODEV companion above is not an
    // accident of the transport always returning 0.
    srvconn_set_client_deadline(cn, timer_now_ns());   // already elapsed -> -1
    int pumped = p9_client_reader_pump_once(&g_sc_client);
    TEST_EXPECT_EQ(pumped, -P9_E_IO, "pump returns DEAD");
    TEST_ASSERT(g_sc_async.completed, "async op completed on the transport error");
    TEST_EXPECT_EQ((u64)(s64)g_sc_async.last_result, (u64)(s64)(-P9_E_IO),
        "transport-error CQE = -EIO (NOT device-gone) over the real srvconn");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-P9_E_IO),
        "the consumer's CQE carries -EIO");

    p9_client_destroy(&g_sc_client);
    (void)ops.close(ops.ctx);
    p9_srvconn_transport_destroy(&st);
    loom_unref(l);
    cleanup_byte_mode_pair(server, client, conn_h);
}
