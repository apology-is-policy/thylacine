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
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/srvconn.h>
#include <thylacine/types.h>

// External declarations -- the byte-mode posting + connect path lives
// in kernel/syscall.c + kernel/devsrv.c.
extern int sys_post_service_byte_for_proc(struct Proc *p, const char *name,
                                            u8 name_len);
extern int srv_conn_open_for_proc(struct Proc *p, const char *name, u8 name_len);
extern void srv_registry_reset(void);

// Forward decls for the registered test entries (suppress -Wmissing-
// prototypes; matches the rest of the kernel test corpus pattern).
void test_9p_srvconn_transport_init_destroy(void);
void test_9p_srvconn_transport_init_null_rejected(void);
void test_9p_srvconn_transport_send_routes_to_c2s_ring(void);
void test_9p_srvconn_transport_recv_routes_from_s2c_ring(void);
void test_9p_srvconn_transport_close_drops_srvconn_ref(void);

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

// Post a byte-mode service, open one connection, return the (server,
// client, srv_handle, conn_handle, cn) tuple via out-params. All test
// procs reset on cleanup -- test should call cleanup_byte_mode_pair on
// the way out.
static struct SrvConn *open_byte_mode_pair(struct Proc **out_server,
                                            struct Proc **out_client,
                                            int *out_svc_h, int *out_conn_h) {
    struct Proc *server = make_marked_test_proc();
    if (!server) return NULL;
    int svc_h = sys_post_service_byte_for_proc(server, "btest", 5);
    if (svc_h < 0) { drop_test_proc(server); return NULL; }

    struct Proc *client = make_test_proc();
    if (!client) { drop_test_proc(server); return NULL; }

    int conn_h = srv_conn_open_for_proc(client, "btest", 5);
    if (conn_h < 0) {
        drop_test_proc(server);
        drop_test_proc(client);
        return NULL;
    }

    struct Handle *slot = handle_get(client, (hidx_t)conn_h);
    if (!slot || slot->kind != KOBJ_SRV || !slot->obj) {
        handle_close(client, (hidx_t)conn_h);
        srv_registry_reset();
        drop_test_proc(server);
        drop_test_proc(client);
        return NULL;
    }
    struct SrvConn *cn = (struct SrvConn *)slot->obj;

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

    // Pre-state. The client's KObj_Srv holds 1 ref. The accept-backlog
    // side dropped its ref when the client opened (per the SrvConn
    // lifecycle, srv_conn_open_for_proc installs the handle which
    // takes the canonical reference). So cn->ref should be 1 here.

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

    // A freshly-minted SrvConn from srv_conn_open_for_proc has THREE
    // holders pre-test:
    //   (a) the client's KObj_Srv handle slot,
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
