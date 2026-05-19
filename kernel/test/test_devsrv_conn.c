// P5-corvus-srv-impl-a3b — kernel-internal tests for the /srv
// per-connection layer: the devsrv walk op, the client-connect path, the
// accept backlog + SYS_SRV_ACCEPT, the connection-Spoor I/O ops, and
// connection teardown.
//
// Coverage:
//
//   devsrv.walk_service
//     A walk of the /srv root with a posted service name yields a service
//     Spoor (QTFILE, a devsrv_svc_ref aux); a walk of an unposted name or
//     two components deep fails; a clone (nname=0) stays a /srv root.
//
//   devsrv.conn_open
//     srv_conn_open_for_proc mints a connection, enqueues it on the accept
//     backlog, and installs a KObj_Srv handle; the handle is
//     non-transferable (handle_dup refuses it); a connect to an unposted
//     or tombstoned service fails.
//
//   devsrv.accept_immediate
//     With a connection already backlogged, SYS_SRV_ACCEPT returns at once
//     with a KObj_Spoor server endpoint and drains the backlog; the
//     endpoint and the client handle name the same SrvConn.
//
//   devsrv.accept_blocks_then_wakes
//     SYS_SRV_ACCEPT on an empty backlog blocks (THREAD_SLEEPING, the
//     accept-rendez waiter); a client connect wakes it.
//
//   devsrv.conn_io
//     corvus reads c2s bytes off its accepted endpoint via SYS_READ and
//     writes s2c bytes via SYS_WRITE; an empty live connection reads 0;
//     closing the endpoint tears the connection down.
//
//   devsrv.conn_release
//     The connection is held by the client handle + corvus's endpoint;
//     closing one tears the transport down but does not free; closing the
//     last reference frees the SrvConn.
//
//   devsrv.poster_exit_drains_backlog
//     srv_proc_exit_notify on the poster tombstones the service AND tears
//     down + drains every pending backlog connection.
//
// Each test calls srv_registry_reset() first so it starts from an empty
// registry (the harness runs tests sequentially in one address space).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Inner syscall cores (non-static; defined in kernel/syscall.c).
extern int sys_post_service_for_proc(struct Proc *p, const char *name,
                                     size_t name_len);
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);

// Test-support registry wipe (non-static; defined in kernel/devsrv.c;
// deliberately not in devsrv.h — no production caller).
extern void srv_registry_reset(void);

void test_devsrv_walk_service(void);
void test_devsrv_conn_open(void);
void test_devsrv_accept_immediate(void);
void test_devsrv_accept_blocks_then_wakes(void);
void test_devsrv_conn_io(void);
void test_devsrv_conn_release(void);
void test_devsrv_poster_exit_drains_backlog(void);

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

// ---------------------------------------------------------------------------
// devsrv.walk_service
// ---------------------------------------------------------------------------

void test_devsrv_walk_service(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "proc_alloc + mark");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");

    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv.attach yields the /srv root");
    TEST_EXPECT_EQ((int)root->qid.type, (int)QTDIR, "/srv root is QTDIR");

    // Walk root → "corvus" yields a service Spoor.
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "spoor_clone for the walk target");
    const char *names[1] = { "corvus" };
    struct Walkqid *w = devsrv.walk(root, nc, names, 1);
    TEST_ASSERT(w != NULL, "walk /srv/corvus succeeds");
    TEST_EXPECT_EQ(w->nqid, 1, "one component walked");
    TEST_EXPECT_EQ(w->spoor, nc, "walk positions the supplied Spoor");
    TEST_EXPECT_EQ((int)nc->qid.type, (int)QTFILE,
        "the service Spoor is a file (QTFILE)");
    struct devsrv_svc_ref *ref = (struct devsrv_svc_ref *)nc->aux;
    TEST_ASSERT(ref != NULL, "the service Spoor carries an aux");
    TEST_EXPECT_EQ(ref->magic, DEVSRV_SVC_MAGIC,
        "the aux is a devsrv_svc_ref");
    TEST_EXPECT_EQ((int)ref->name_len, 6, "the aux records the service name");
    walkqid_free(w);
    spoor_clunk(nc);                       // → devsrv_close frees the aux

    // A walk of a name with no LIVE service → NULL.
    struct Spoor *nc2 = spoor_clone(root);
    TEST_ASSERT(nc2 != NULL, "spoor_clone");
    const char *miss[1] = { "absent" };
    TEST_ASSERT(devsrv.walk(root, nc2, miss, 1) == NULL,
        "walk of an unposted name → NULL");
    spoor_clunk(nc2);

    // A walk deeper than one component → NULL (no /srv/<name>/<path> yet).
    struct Spoor *nc3 = spoor_clone(root);
    TEST_ASSERT(nc3 != NULL, "spoor_clone");
    const char *deep[2] = { "corvus", "ctl" };
    TEST_ASSERT(devsrv.walk(root, nc3, deep, 2) == NULL,
        "walk two components deep → NULL");
    spoor_clunk(nc3);

    // A clone (nname == 0) → the supplied Spoor stays a /srv root (no aux).
    struct Spoor *nc4 = spoor_clone(root);
    TEST_ASSERT(nc4 != NULL, "spoor_clone");
    struct Walkqid *wc = devsrv.walk(root, nc4, NULL, 0);
    TEST_ASSERT(wc != NULL, "clone walk (nname=0) succeeds");
    TEST_EXPECT_EQ(wc->nqid, 0, "clone walks zero components");
    TEST_ASSERT(nc4->aux == NULL, "the cloned root Spoor carries no aux");
    walkqid_free(wc);
    spoor_clunk(nc4);

    spoor_clunk(root);
    srv_registry_reset();
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.conn_open
// ---------------------------------------------------------------------------

void test_devsrv_conn_open(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");
    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "the service is registered");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    // A client opens /srv/corvus.
    int h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(h >= 0, "srv_conn_open → a connection handle");
    struct Handle *sh = handle_get(client, h);
    TEST_ASSERT(sh != NULL, "the connection handle is installed");
    TEST_EXPECT_EQ((int)sh->kind, (int)KOBJ_SRV,
        "the connection handle is KObj_Srv");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 1,
        "the connection is enqueued on the accept backlog");

    // KObj_Srv is non-transferable — handle_dup must refuse it (NoSrvDup).
    TEST_EXPECT_EQ(handle_dup(client, h, RIGHT_READ), -1,
        "dup of a KObj_Srv connection handle → -1");

    // A connect to a name with no LIVE service → -1.
    TEST_EXPECT_EQ(srv_conn_open_for_proc(client, "absent", 6), -1,
        "connect to an unposted name → -1");

    // A connect to a TOMBSTONED service → -1 (the drain also clears the
    // one backlogged connection).
    srv_proc_exit_notify(corvus);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "the poster exit tombstoned the service");
    TEST_EXPECT_EQ(srv_conn_open_for_proc(client, "corvus", 6), -1,
        "connect to a tombstoned service → -1");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.accept_immediate
// ---------------------------------------------------------------------------

void test_devsrv_accept_immediate(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");
    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "the service is registered");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    // The client connects first — the connection waits on the backlog.
    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 1, "one connection backlogged");

    // corvus accepts — the backlog is non-empty, so accept returns at once.
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "accept returns a connection handle");
    struct Handle *ch = handle_get(corvus, conn_h);
    TEST_ASSERT(ch != NULL, "the accepted handle is installed");
    TEST_EXPECT_EQ((int)ch->kind, (int)KOBJ_SPOOR,
        "the accepted handle is a KObj_Spoor server endpoint");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 0, "accept drained the backlog");

    // The accepted endpoint and the client's handle name the same SrvConn.
    struct SrvConn *cn_srv = (struct SrvConn *)((struct Spoor *)ch->obj)->aux;
    struct SrvConn *cn_cli =
        (struct SrvConn *)handle_get(client, client_h)->obj;
    TEST_EXPECT_EQ(cn_srv, cn_cli,
        "accept and connect name the same connection");

    // A stranger Proc cannot accept the service it never posted.
    struct Proc *stranger = make_marked_test_proc();
    TEST_ASSERT(stranger != NULL, "stranger proc");
    TEST_EXPECT_EQ(sys_srv_accept_for_proc(stranger, (hidx_t)svc_h), -1,
        "a non-poster cannot accept the service");

    srv_registry_reset();
    drop_test_proc(stranger);
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.accept_blocks_then_wakes — threaded.
// ---------------------------------------------------------------------------

static struct Proc *g_da_corvus;
static hidx_t       g_da_svc_h;
static volatile u32 g_da_ran;
static volatile s64 g_da_ret;

static void devsrv_accept_worker(void) {
    g_da_ran++;                                          // → 1: pre-accept
    g_da_ret = sys_srv_accept_for_proc(g_da_corvus, g_da_svc_h);
    g_da_ran++;                                          // → 2: post-accept
    for (;;) sched();                                    // park safely
}

void test_devsrv_accept_blocks_then_wakes(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");
    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "the service is registered");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    g_da_corvus = corvus;
    g_da_svc_h  = (hidx_t)svc_h;
    g_da_ran    = 0;
    g_da_ret    = -999;

    TEST_EXPECT_EQ(sched_runnable_count(), 0u,
        "run tree must be empty at test entry");

    struct Thread *worker = thread_create(kproc(), devsrv_accept_worker);
    TEST_ASSERT(worker != NULL, "thread_create(accept worker)");
    ready(worker);

    // Yield: the worker runs, then blocks in accept on the empty backlog.
    sched();
    TEST_EXPECT_EQ(g_da_ran, 1u, "the worker ran once before blocking");
    TEST_EXPECT_EQ(worker->state, THREAD_SLEEPING,
        "the worker is SLEEPING inside the accept");
    TEST_EXPECT_EQ(svc->accept_rendez.waiter, worker,
        "the worker is the accept-rendez waiter");

    // A client connects — the enqueue wakes the blocked accepter.
    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");

    sched();
    TEST_EXPECT_EQ(g_da_ran, 2u, "the worker resumed past the accept");
    TEST_ASSERT(g_da_ret >= 0, "the accept returned a connection handle");

    // The accepted endpoint names the connection the client opened.
    struct Handle *ch = handle_get(corvus, (hidx_t)g_da_ret);
    TEST_ASSERT(ch != NULL, "the accepted handle is installed");
    struct SrvConn *cn_srv = (struct SrvConn *)((struct Spoor *)ch->obj)->aux;
    struct SrvConn *cn_cli =
        (struct SrvConn *)handle_get(client, client_h)->obj;
    TEST_EXPECT_EQ(cn_srv, cn_cli,
        "the accept woke onto the client's connection");

    thread_free(worker);
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.conn_io
// ---------------------------------------------------------------------------

void test_devsrv_conn_io(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    struct SrvConn *cn = (struct SrvConn *)handle_get(client, client_h)->obj;
    TEST_ASSERT(cn != NULL, "the client handle names a SrvConn");

    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // The kernel 9P client side queues bytes on c2s; corvus reads them off
    // its server endpoint via SYS_READ → devsrv_read → srvconn_server_recv.
    u8 msg[16];
    for (int i = 0; i < 16; i++) msg[i] = (u8)(0x40 + i);
    TEST_EXPECT_EQ(srvconn_client_send(cn, msg, 16), 16L,
        "the kernel-client side queues 16 bytes toward corvus");
    u8 got[16];
    TEST_EXPECT_EQ(sys_read_for_proc(corvus, (hidx_t)conn_h, got, 16), 16,
        "corvus reads the 16 bytes off its accepted endpoint");
    bool ok = true;
    for (int i = 0; i < 16; i++) if (got[i] != (u8)(0x40 + i)) ok = false;
    TEST_ASSERT(ok, "the request bytes round-tripped intact");

    // An empty live connection reads 0 — corvus polls again.
    TEST_EXPECT_EQ(sys_read_for_proc(corvus, (hidx_t)conn_h, got, 16), 0,
        "an empty live connection reads 0");

    // corvus writes a server→client frame; the kernel-client side reads it.
    u8 reply[8];
    for (int i = 0; i < 8; i++) reply[i] = (u8)(0x90 + i);
    TEST_EXPECT_EQ(sys_write_for_proc(corvus, (hidx_t)conn_h, reply, 8), 8,
        "corvus writes 8 bytes toward the client");
    u8 cli[8];
    TEST_EXPECT_EQ(srvconn_client_recv(cn, cli, 8), 8L,
        "the kernel-client side reads corvus's 8 bytes");
    ok = true;
    for (int i = 0; i < 8; i++) if (cli[i] != (u8)(0x90 + i)) ok = false;
    TEST_ASSERT(ok, "the reply bytes round-tripped intact");

    // Closing corvus's endpoint tears the connection down.
    TEST_EXPECT_EQ(handle_close(corvus, (hidx_t)conn_h), 0,
        "corvus closes its endpoint");
    TEST_ASSERT(srvconn_is_live(cn) == false,
        "closing the endpoint tore the connection down");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.conn_release
// ---------------------------------------------------------------------------

void test_devsrv_conn_release(void) {
    srv_registry_reset();
    u64 created0 = srvconn_total_created();
    u64 freed0   = srvconn_total_freed();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    TEST_EXPECT_EQ(srvconn_total_created(), created0 + 1,
        "the connect minted one SrvConn");

    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");
    struct SrvConn *cn = (struct SrvConn *)handle_get(client, client_h)->obj;
    TEST_EXPECT_EQ(cn->ref, 2,
        "the connection is held by the client handle and corvus's endpoint");

    // Closing the client handle is one connection close: teardown + one
    // unref. corvus still holds the endpoint, so the SrvConn is not freed.
    TEST_EXPECT_EQ(handle_close(client, (hidx_t)client_h), 0,
        "the client closes its connection handle");
    TEST_EXPECT_EQ(srvconn_total_freed(), freed0,
        "the SrvConn is not freed while corvus holds the endpoint");
    TEST_ASSERT(srvconn_is_live(cn) == false,
        "the connection close tore the transport down");
    TEST_EXPECT_EQ(cn->ref, 1, "one reference remains (corvus's endpoint)");

    // Closing corvus's endpoint drops the last reference → free.
    TEST_EXPECT_EQ(handle_close(corvus, (hidx_t)conn_h), 0,
        "corvus closes its endpoint");
    TEST_EXPECT_EQ(srvconn_total_freed(), freed0 + 1,
        "the last close frees the SrvConn");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.poster_exit_drains_backlog
// ---------------------------------------------------------------------------

void test_devsrv_poster_exit_drains_backlog(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    TEST_ASSERT(sys_post_service_for_proc(corvus, "corvus", 6) >= 0,
        "post \"corvus\"");
    struct SrvService *svc = srv_lookup("corvus", 6);
    TEST_ASSERT(svc != NULL, "the service is registered");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");

    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 1, "one connection backlogged");
    struct SrvConn *cn = (struct SrvConn *)handle_get(client, client_h)->obj;
    TEST_ASSERT(srvconn_is_live(cn),
        "the connection is live before the poster exits");

    // The poster exits before accepting — the backlogged connection has no
    // server. srv_proc_exit_notify tombstones the name AND tears down +
    // drains the pending connection.
    srv_proc_exit_notify(corvus);
    TEST_EXPECT_EQ((int)svc->state, (int)SRV_STATE_TOMBSTONED,
        "the poster exit tombstoned the service");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 0,
        "the poster exit drained the accept backlog");
    TEST_ASSERT(srvconn_is_live(cn) == false,
        "the drained connection was torn down (its client wakes with EOF)");

    // The client still holds its handle, so the SrvConn is not yet freed;
    // closing the (now-dead) handle drops the last reference.
    TEST_EXPECT_EQ(handle_close(client, (hidx_t)client_h), 0,
        "the client closes its now-dead connection handle");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}
