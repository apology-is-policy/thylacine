// kernel-internal tests for the /srv CLIENT side: the byte-mode KObj_Srv
// r/w dispatch in SYS_READ / SYS_WRITE, byte_mode propagation from the
// service, and the SYS_SRV_CONNECT entry-point gates.
//
// stalk-3b-β retired the embedded per-SrvConn 9P client (the connect path
// for 9P services is now open=connect -> devsrv_open_connect -> a dev9p
// root Spoor; SYS_SRV_CONNECT is byte-mode only) AND removed the per-Proc
// connection cap (3a-audit F4). The per-Proc-cap tests + the embedded-9P-
// client handshake-drive tests this file once carried are gone with the
// mechanisms they exercised.
//
// What this file covers now:
//
//   srv_client.no_per_proc_cap
//     Two byte connections from ONE client Proc both succeed (the
//     per-Proc cap was removed — a session needs corvus AND its
//     stratum-fs concurrently).
//
//   srv_client.sys_srv_connect_unknown_service
//     SYS_SRV_CONNECT against a name that no Proc has posted returns -1
//     with no SrvConn / handle created.
//
//   srv_client.sys_srv_connect_9p_rejected
//     SYS_SRV_CONNECT against a 9P-mode service returns -1 (the byte-only
//     surface rejects 9P; 9P is reached via open=connect).
//
//   srv_client.byte_mode_propagates_to_conn / _9p_post_stays_9p
//     A SrvConn's byte_mode mirrors the service's posted mode.
//
//   srv_client.byte_mode_connect_rejects_path
//     A byte-mode SYS_SRV_CONNECT with a non-empty path returns -1.
//
//   srv_client.byte_mode_kobj_srv_dispatch
//     The KObj_Srv r/w arm round-trips raw bytes through
//     srvconn_client_send/recv (no 9P framing).
//
//   srv_client.byte_mode_mode_change_rebind_refused
//     A tombstoned service cannot be rebound with a different mode.
//
//   srv_client.byte_mode_server_recv_blocking_eof
//     srvconn_server_recv_blocking returns 0 (EOF) on empty + torn.

#include "test.h"

#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Testable cores defined in kernel/syscall.c (non-static — same pattern
// as the other -for_proc helpers).
extern int sys_post_service_for_proc(struct Proc *p, const char *name,
                                     size_t name_len);
extern int sys_post_service_byte_for_proc(struct Proc *p, const char *name,
                                           size_t name_len);
extern int sys_srv_connect_for_proc(struct Proc *p,
                                    const char *name, size_t name_len,
                                    const u8 *path, size_t path_len);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);

// Test-support: registry wipe (non-static in kernel/devsrv.c, no
// production caller).
extern void srv_registry_reset(void);

void test_srv_client_no_per_proc_cap(void);
void test_srv_client_sys_srv_connect_unknown_service(void);
void test_srv_client_sys_srv_connect_9p_rejected(void);
void test_srv_client_byte_mode_propagates_to_conn(void);
void test_srv_client_byte_mode_9p_post_stays_9p(void);
void test_srv_client_byte_mode_connect_rejects_path(void);
void test_srv_client_byte_mode_kobj_srv_dispatch(void);
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

// Resolve a KOBJ_SRV handle to its underlying SrvConn (for tests that
// need to read byte_mode directly). Returns NULL on a bad / non-SRV /
// wrong-magic handle.
static struct SrvConn *conn_of(struct Proc *p, hidx_t h) {
    struct Handle *slot = handle_get(p, h);
    if (!slot || slot->kind != KOBJ_SRV || !slot->obj) return NULL;
    if (*(const u64 *)slot->obj != SRV_CONN_MAGIC)     return NULL;
    return (struct SrvConn *)slot->obj;
}

// ---------------------------------------------------------------------------
// srv_client.no_per_proc_cap
// ---------------------------------------------------------------------------
//
// stalk-3b-β removed the per-Proc cap (3a-audit F4). Two connections from
// ONE Proc both succeed; the accept backlog (16) and the global
// SRV_MAX_CONNS soft cap remain the only resource bounds.

void test_srv_client_no_per_proc_cap(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_byte_for_proc(server, "sockd", 5);
    TEST_ASSERT(svc_h >= 0, "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int c1 = srv_conn_open_for_proc(client, "sockd", 5);
    TEST_ASSERT(c1 >= 0, "first byte connection opens");

    // Second open from the SAME Proc — previously refused by the per-Proc
    // cap (1); now succeeds.
    int c2 = srv_conn_open_for_proc(client, "sockd", 5);
    TEST_ASSERT(c2 >= 0,
        "second connection from the same Proc also opens (no per-Proc cap)");
    TEST_ASSERT(c1 != c2, "the two connections are distinct handles");

    handle_close(client, c1);
    handle_close(client, c2);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.sys_srv_connect_unknown_service
// ---------------------------------------------------------------------------

void test_srv_client_sys_srv_connect_unknown_service(void) {
    srv_registry_reset();

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "proc_alloc");

    // No service posted under "corvus" — registry is empty.
    int rc = sys_srv_connect_for_proc(client, "corvus", 6, NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "SYS_SRV_CONNECT to unposted name → -1");

    srv_registry_reset();
    drop_test_proc(client);
}

// ---------------------------------------------------------------------------
// srv_client.sys_srv_connect_9p_rejected
// ---------------------------------------------------------------------------
//
// SYS_SRV_CONNECT is byte-mode only post stalk-3b-β. A 9P-mode service is
// reached via open=connect (devsrv_open_connect drives Tversion + Tattach
// and returns a dev9p root Spoor), NOT via SYS_SRV_CONNECT. The handler
// mints the SrvConn, observes !byte_mode, and fails-closed (closing the
// just-installed handle).

void test_srv_client_sys_srv_connect_9p_rejected(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_for_proc(server, "ninep", 5);
    TEST_ASSERT(svc_h >= 0, "post \"ninep\" in default (9P) mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int rc = sys_srv_connect_for_proc(client, "ninep", 5, NULL, 0);
    TEST_EXPECT_EQ(rc, -1,
        "SYS_SRV_CONNECT to a 9P-mode service is rejected (use open=connect)");

    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

// =============================================================================
// P6-pouch-sockets (sub-chunk 12) — SRV_MODE_BYTE byte-stream transport
// =============================================================================
//
//   srv_client.byte_mode_propagates_to_conn
//     A SrvConn minted against a SYS_POST_SERVICE_BYTE-posted service
//     carries byte_mode = true.
//
//   srv_client.byte_mode_9p_post_stays_9p
//     A SrvConn minted against the legacy SYS_POST_SERVICE-posted
//     service carries byte_mode = false. corvus + stratumd unaffected.
//
//   srv_client.byte_mode_connect_rejects_path
//     SYS_srv_connect against a byte-mode service WITH path_len > 0
//     returns -1. Byte mode has no 9P fid to walk against.
//
//   srv_client.byte_mode_kobj_srv_dispatch
//     sys_read/write_for_proc's KObj_SRV arm dispatches a byte-mode
//     write through srvconn_client_send (raw chan_produce on c2s) —
//     server-side read drains byte-identical bytes.

void test_srv_client_byte_mode_propagates_to_conn(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_byte_for_proc(server, "sockd", 5);
    TEST_ASSERT(svc_h >= 0, "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int conn_h = srv_conn_open_for_proc(client, "sockd", 5);
    TEST_ASSERT(conn_h >= 0, "client opens the byte-mode service");

    struct SrvConn *cn = conn_of(client, conn_h);
    TEST_ASSERT(cn != NULL, "conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1,
        "byte-mode service produces byte-mode SrvConn");

    handle_close(client, conn_h);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

void test_srv_client_byte_mode_9p_post_stays_9p(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_for_proc(server, "ninep", 5);
    TEST_ASSERT(svc_h >= 0, "post \"ninep\" in default (9P) mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int conn_h = srv_conn_open_for_proc(client, "ninep", 5);
    TEST_ASSERT(conn_h >= 0, "client opens the 9P-mode service");

    struct SrvConn *cn = conn_of(client, conn_h);
    TEST_ASSERT(cn != NULL, "conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 0,
        "9P-mode service produces a non-byte-mode SrvConn (legacy path)");

    handle_close(client, conn_h);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

void test_srv_client_byte_mode_connect_rejects_path(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_byte_for_proc(server, "sockd", 5);
    TEST_ASSERT(svc_h >= 0, "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    const u8 path[] = "ctl";
    int rc = sys_srv_connect_for_proc(client, "sockd", 5,
                                       path, sizeof(path) - 1);
    TEST_EXPECT_EQ(rc, -1, "byte-mode connect rejects non-empty path");

    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}

void test_srv_client_byte_mode_kobj_srv_dispatch(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    int svc_h = sys_post_service_byte_for_proc(server, "sockd", 5);
    TEST_ASSERT(svc_h >= 0, "post \"sockd\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int conn_h = srv_conn_open_for_proc(client, "sockd", 5);
    TEST_ASSERT(conn_h >= 0, "client opens the byte-mode service");

    struct SrvConn *cn = conn_of(client, conn_h);
    TEST_ASSERT(cn != NULL, "conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1, "byte_mode set");

    int conn_spoor_h = sys_srv_accept_for_proc(server, (hidx_t)svc_h);
    TEST_ASSERT(conn_spoor_h >= 0, "server accepts the byte-mode conn");

    // Client write via sys_write_for_proc on KObj_SRV. The arm routes
    // through srvconn_client_send — raw c2s push. NO 9P Twrite framing.
    const u8 client_msg[5] = { 'P', 'I', 'N', 'G', '\n' };
    s64 w = sys_write_for_proc(client, (hidx_t)conn_h, client_msg, 5);
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
    s64 cr = sys_read_for_proc(client, (hidx_t)conn_h, cli_recv, 16);
    TEST_EXPECT_EQ(cr, 5, "byte-mode client read returns exactly 5 bytes");
    exact = true;
    for (int i = 0; i < 5; i++) if (cli_recv[i] != server_msg[i]) exact = false;
    TEST_ASSERT(exact, "byte-mode byte-accuracy: PONG\\n round-tripped");

    handle_close(server, conn_spoor_h);
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
    int svc_h = sys_post_service_byte_for_proc(server1, "sockmix", 7);
    TEST_ASSERT(svc_h >= 0, "post \"sockmix\" in byte mode");

    // Tombstone via srv_proc_exit_notify (mirrors poster-exit path).
    srv_proc_exit_notify(server1);
    struct SrvService *svc = srv_lookup("sockmix", 7);
    TEST_ASSERT(svc != NULL, "tombstoned entry persists");
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "poster exit tombstoned the entry");

    // Phase 2: a marked Proc tries to rebind in 9P mode — REFUSED.
    struct Proc *server2 = make_marked_test_proc();
    TEST_ASSERT(server2 != NULL, "server2 proc");
    TEST_EXPECT_EQ(sys_post_service_for_proc(server2, "sockmix", 7), -1,
        "rebind with different mode (9P over byte tombstone) → -1");
    svc = srv_lookup("sockmix", 7);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "refused mode-change rebind left tombstone intact");

    // Phase 3: a same-mode rebind (byte → byte) SUCCEEDS — F2 only
    // refuses MODE CHANGES, not all rebinds.
    TEST_ASSERT(sys_post_service_byte_for_proc(server2, "sockmix", 7) >= 0,
        "same-mode rebind (byte → byte) → handle");
    svc = srv_lookup("sockmix", 7);
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
    int svc_h = sys_post_service_byte_for_proc(server, "sockeof", 7);
    TEST_ASSERT(svc_h >= 0, "post \"sockeof\" in byte mode");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int conn_h = srv_conn_open_for_proc(client, "sockeof", 7);
    TEST_ASSERT(conn_h >= 0, "client opens the byte-mode service");

    struct SrvConn *cn = conn_of(client, conn_h);
    TEST_ASSERT(cn != NULL, "conn_of recovers the SrvConn");
    TEST_EXPECT_EQ((int)cn->byte_mode, 1, "byte_mode set");

    // Tear the connection down — chan_set_eof on c2s + s2c.
    srvconn_teardown(cn);

    // Server endpoint blocking recv on empty + eof should return 0
    // (EOF) immediately, not block. This is the finite case of the
    // F1 fix — the eof-latched short-circuit at the top of the loop.
    u8 buf[16];
    long r = srvconn_server_recv_blocking(cn, buf, 16);
    TEST_EXPECT_EQ(r, 0L,
        "srvconn_server_recv_blocking returns 0 on empty + eof");

    handle_close(client, conn_h);
    srv_registry_reset();
    drop_test_proc(server);
    drop_test_proc(client);
}
