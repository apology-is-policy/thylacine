// kernel-internal tests for the /srv CLIENT side: byte-mode connection
// minting (open=connect), byte_mode propagation from the service, and the
// byte-stream r/w round-trip through the connection-endpoint Spoors.
//
// stalk-3c retired the name-only SYS_SRV_CONNECT / SYS_POST_SERVICE[_BYTE]
// syscalls: posting is SYS_WALK_CREATE on a /srv dir (create=post ->
// devsrv_post_listener) and connecting is SYS_OPEN on /srv/<name>
// (open=connect -> devsrv_open_connect). A byte-mode connect returns a
// CLIENT-direction conn-endpoint Spoor (KOBJ_SPOOR, CSRVCLIENT); a 9P-mode
// connect (corvus) drives a Tversion/Tattach handshake and returns a dev9p
// root Spoor (exercised end-to-end at boot, not here -- a unit test has no
// server thread to answer the handshake). These tests drive the production
// cores (devsrv_post_listener / devsrv_open_connect) directly, the same
// machinery stalk reaches.
//
// Coverage:
//
//   srv_client.no_per_proc_cap
//     Two byte connections from ONE client Proc both succeed (the
//     per-Proc cap was removed in stalk-3b-beta -- a session needs corvus
//     AND its stratum-fs concurrently).
//
//   srv_client.byte_mode_propagates_to_conn
//     A SrvConn minted against a byte-mode service carries byte_mode = true.
//
//   srv_client.byte_mode_conn_dispatch
//     sys_read/write_for_proc's KOBJ_SPOOR arm dispatches a byte-mode conn
//     endpoint through devsrv read/write -> srvconn_client_send/recv (client
//     side) and srvconn_server_recv/send (the accepted server side) -- a raw
//     byte round-trip with no 9P framing.
//
//   srv_client.byte_mode_mode_change_rebind_refused
//     A tombstoned service cannot be rebound with a different mode.
//
//   srv_client.byte_mode_server_recv_blocking_eof
//     srvconn_server_recv_blocking returns 0 (EOF) on empty + torn.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Read/write + accept cores defined in kernel/syscall.c (non-static).
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);

// Test-support: registry wipe (non-static in kernel/devsrv.c, no
// production caller).
extern void srv_registry_reset(void);

void test_srv_client_no_per_proc_cap(void);
void test_srv_client_byte_mode_propagates_to_conn(void);
void test_srv_client_byte_mode_conn_dispatch(void);
void test_srv_client_byte_mode_mode_change_rebind_refused(void);
void test_srv_client_byte_mode_server_recv_blocking_eof(void);

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static struct Proc *make_test_proc(void) {
    return proc_alloc();
}

static struct Proc *make_marked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_mark_may_post_service(p);
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// post_svc_byte / post_svc_9p — post a service into the boot registry via
// the production create=post path (devsrv_post_listener on a transient boot
// /srv root). Returns the listener handle (>= 0) or -1.
static int post_svc_byte(struct Proc *p, const char *name, size_t name_len) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return -1;
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_BYTE, false);
    spoor_clunk(root);
    return h;
}

static int post_svc_9p(struct Proc *p, const char *name, size_t name_len) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return -1;
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_9P, false);
    spoor_clunk(root);
    return h;
}

// connect_byte — open=connect to a byte-mode /srv service: walk /srv/<name>
// to a service-ref Spoor, then devsrv_open_connect -> a CLIENT-direction
// byte-conn endpoint Spoor (CSRVCLIENT). Returns the conn Spoor or NULL; the
// caller owns it (wrap in a KOBJ_SPOOR handle or spoor_clunk). `name` must be
// NUL-terminated (the devsrv walk reads it as a C string).
static struct Spoor *connect_byte(struct Proc *p, const char *name) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return NULL;
    struct Spoor *sref = spoor_clone(root);
    if (!sref) { spoor_clunk(root); return NULL; }
    const char *names[1] = { name };
    struct Walkqid *w = devsrv.walk(root, sref, names, 1);
    if (!w) { spoor_clunk(sref); spoor_clunk(root); return NULL; }
    walkqid_free(w);
    struct Spoor *cs = devsrv_open_connect(p, sref, /*omode ORDWR*/ 2);
    spoor_clunk(sref);                 // the spent quarry (open-returns-new)
    spoor_clunk(root);
    return cs;
}

// ---------------------------------------------------------------------------
// srv_client.no_per_proc_cap
// ---------------------------------------------------------------------------
//
// stalk-3b-beta removed the per-Proc cap (3a-audit F4). Two connections from
// ONE Proc both succeed; the accept backlog (16) and the global
// SRV_MAX_CONNS soft cap remain the only resource bounds.

void test_srv_client_no_per_proc_cap(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    TEST_ASSERT(post_svc_byte(server, "sockd", 5) >= 0,
        "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    struct Spoor *c1 = connect_byte(client, "sockd");
    TEST_ASSERT(c1 != NULL, "first byte connection opens");

    // Second open from the SAME Proc -- previously refused by the per-Proc
    // cap (1); now succeeds.
    struct Spoor *c2 = connect_byte(client, "sockd");
    TEST_ASSERT(c2 != NULL,
        "second connection from the same Proc also opens (no per-Proc cap)");
    TEST_ASSERT(c1 != c2, "the two connections are distinct endpoints");

    spoor_clunk(c1);
    spoor_clunk(c2);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.byte_mode_propagates_to_conn
// ---------------------------------------------------------------------------

void test_srv_client_byte_mode_propagates_to_conn(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    TEST_ASSERT(post_svc_byte(server, "sockd", 5) >= 0,
        "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    struct Spoor *cs = connect_byte(client, "sockd");
    TEST_ASSERT(cs != NULL, "client opens the byte-mode service");
    struct SrvConn *cn = devsrv_conn_of(cs);
    TEST_ASSERT(cn != NULL, "devsrv_conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1,
        "byte-mode service produces byte-mode SrvConn");

    spoor_clunk(cs);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.byte_mode_conn_dispatch
// ---------------------------------------------------------------------------
//
// The byte client endpoint (open=connect -> a CSRVCLIENT conn Spoor) and the
// accepted server endpoint (SYS_SRV_ACCEPT -> a KOBJ_SPOOR conn Spoor) both
// route SYS_READ / SYS_WRITE through the KOBJ_SPOOR arm -> devsrv read/write,
// whose CSRVCLIENT vs server branch picks srvconn_client_* vs srvconn_server_*
// (raw c2s/s2c, no 9P framing). The pre-stalk-3c KOBJ_SRV client r/w arm is
// retired with SYS_SRV_CONNECT.

void test_srv_client_byte_mode_conn_dispatch(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = post_svc_byte(server, "sockd", 5);
    TEST_ASSERT(svc_h >= 0, "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    struct Spoor *cs = connect_byte(client, "sockd");
    TEST_ASSERT(cs != NULL, "client opens the byte-mode service");
    struct SrvConn *cn = devsrv_conn_of(cs);
    TEST_ASSERT(cn != NULL, "devsrv_conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1, "byte_mode set");

    // Install the client endpoint as a KOBJ_SPOOR handle -- the production
    // shape (stalk's SYS_OPEN of /srv/<name> returns a KOBJ_SPOOR conn Spoor).
    hidx_t conn_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE,
                                 cs);
    TEST_ASSERT(conn_h >= 0,
        "install the client conn endpoint as a KOBJ_SPOOR handle");

    int conn_spoor_h = sys_srv_accept_for_proc(server, (hidx_t)svc_h);
    TEST_ASSERT(conn_spoor_h >= 0, "server accepts the byte-mode conn");

    // Client write -> KOBJ_SPOOR arm -> devsrv_write CSRVCLIENT branch ->
    // srvconn_client_send (raw c2s). NO 9P Twrite framing.
    const u8 client_msg[5] = { 'P', 'I', 'N', 'G', '\n' };
    s64 w = sys_write_for_proc(client, conn_h, client_msg, 5);
    TEST_EXPECT_EQ(w, 5, "byte-mode client write places 5 bytes on c2s");

    u8 srv_recv[16];
    s64 r = sys_read_for_proc(server, (hidx_t)conn_spoor_h, srv_recv, 16);
    TEST_EXPECT_EQ(r, 5,
        "byte-mode server read returns exactly the 5 bytes (no 9P header)");
    bool exact = true;
    for (int i = 0; i < 5; i++) if (srv_recv[i] != client_msg[i]) exact = false;
    TEST_ASSERT(exact, "byte-mode byte-accuracy: PING\\n round-tripped");

    const u8 server_msg[5] = { 'P', 'O', 'N', 'G', '\n' };
    s64 sw = sys_write_for_proc(server, (hidx_t)conn_spoor_h, server_msg, 5);
    TEST_EXPECT_EQ(sw, 5, "byte-mode server write places 5 bytes on s2c");
    u8 cli_recv[16];
    s64 cr = sys_read_for_proc(client, conn_h, cli_recv, 16);
    TEST_EXPECT_EQ(cr, 5, "byte-mode client read returns exactly 5 bytes");
    exact = true;
    for (int i = 0; i < 5; i++) if (cli_recv[i] != server_msg[i]) exact = false;
    TEST_ASSERT(exact, "byte-mode byte-accuracy: PONG\\n round-tripped");

    handle_close(server, (hidx_t)conn_spoor_h);
    handle_close(client, conn_h);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// F2 regression (P6-pouch-sockets audit): mode-changing rebind of a
// tombstoned service is refused. A SrvService's mode is part of its
// identity; a client that captured `service_mode` under the LIVE check
// could otherwise observe a wrong-mode connection landing in the new
// poster's backlog.
// ---------------------------------------------------------------------------

void test_srv_client_byte_mode_mode_change_rebind_refused(void) {
    srv_registry_reset();

    // Phase 1: post in byte mode + commit.
    struct Proc *server1 = make_marked_test_proc();
    TEST_ASSERT(server1 != NULL, "server1 proc");
    TEST_ASSERT(post_svc_byte(server1, "sockmix", 7) >= 0,
        "post \"sockmix\" in byte mode");

    // Tombstone via srv_proc_exit_notify (mirrors poster-exit path).
    srv_proc_exit_notify(server1);
    struct SrvService *svc = srv_lookup_in(srv_boot_registry(), "sockmix", 7);
    TEST_ASSERT(svc != NULL, "tombstoned entry persists");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "poster exit tombstoned the entry");

    // Phase 2: a marked Proc tries to rebind in 9P mode -- REFUSED.
    struct Proc *server2 = make_marked_test_proc();
    TEST_ASSERT(server2 != NULL, "server2 proc");
    TEST_EXPECT_EQ(post_svc_9p(server2, "sockmix", 7), -1,
        "rebind with different mode (9P over byte tombstone) -> -1");
    svc = srv_lookup_in(srv_boot_registry(), "sockmix", 7);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "refused mode-change rebind left tombstone intact");

    // Phase 3: a same-mode rebind (byte -> byte) SUCCEEDS -- F2 only
    // refuses MODE CHANGES, not all rebinds.
    TEST_ASSERT(post_svc_byte(server2, "sockmix", 7) >= 0,
        "same-mode rebind (byte -> byte) -> handle");
    svc = srv_lookup_in(srv_boot_registry(), "sockmix", 7);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_LIVE,
        "same-mode rebind brought the entry LIVE");

    drop_test_proc(server2);
    drop_test_proc(server1);
    srv_registry_reset();
}

// ---------------------------------------------------------------------------
// F1 partial regression (P6-pouch-sockets audit): srvconn_server_recv_
// blocking returns 0 (EOF) when c2s is empty AND eof is latched. The
// full empty-but-live blocking case requires multi-threaded test infra
// (the call would block forever); the empty-eof case is the finite
// regression that confirms the blocking helper teardowns cleanly when
// a peer closes.
// ---------------------------------------------------------------------------

void test_srv_client_byte_mode_server_recv_blocking_eof(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    TEST_ASSERT(post_svc_byte(server, "sockeof", 7) >= 0,
        "post \"sockeof\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    struct Spoor *cs = connect_byte(client, "sockeof");
    TEST_ASSERT(cs != NULL, "client opens the byte-mode service");
    struct SrvConn *cn = devsrv_conn_of(cs);
    TEST_ASSERT(cn != NULL, "devsrv_conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1, "byte_mode set");

    // Tear the connection down -- chan_set_eof on c2s + s2c.
    srvconn_teardown(cn);

    // Server endpoint blocking recv on empty + eof should return 0
    // (EOF) immediately, not block. This is the finite case of the
    // F1 fix -- the eof-latched short-circuit at the top of the loop.
    u8 buf[16];
    long r = srvconn_server_recv_blocking(cn, buf, 16);
    TEST_EXPECT_EQ(r, 0L,
        "srvconn_server_recv_blocking returns 0 on empty + eof");

    spoor_clunk(cs);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}
