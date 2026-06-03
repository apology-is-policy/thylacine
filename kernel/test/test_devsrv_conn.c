// P5-corvus-srv-impl-a3b / -a3c — kernel-internal tests for the /srv
// per-connection layer: the devsrv walk op, the client-connect path, the
// accept backlog + SYS_SRV_ACCEPT, the connection-Spoor I/O ops,
// connection teardown (a3b), and SYS_SRV_PEER (a3c).
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
//   devsrv.srv_peer_identity
//     SYS_SRV_PEER returns the connection's kernel-stamped peer identity
//     (stripes, console bit, caps); the caps read is live — a mutation of
//     the peer's caps shows on the next call.
//
//   devsrv.srv_peer_dead_peer
//     The dead-Proc guard: a zombie / reaped peer fail-closes caps and the
//     alive bit to 0, while the immutable stripes survives.
//
//   devsrv.srv_peer_gate
//     Only the service poster may query a connection's peer — a non-poster,
//     even one holding the connection endpoint, is refused; the endpoint
//     must be a KObj_Spoor.
//
//   devsrv.srv_peer_bad_args
//     SYS_SRV_PEER rejects a NULL Proc / NULL out / bad handle.
//
// Each test calls srv_registry_reset() first so it starts from an empty
// registry (the harness runs tests sequentially in one address space).
// The srv_peer tests link their client Proc into the process table
// (make_linked_test_proc) so the live-caps scan can find it.

#include "test.h"

#include <thylacine/caps.h>
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

// Inner syscall cores (non-static; defined in kernel/syscall.c).
extern int sys_post_service_for_proc(struct Proc *p, const char *name,
                                     size_t name_len);
extern int sys_post_service_byte_for_proc(struct Proc *p, const char *name,
                                          size_t name_len);
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);
extern int sys_srv_peer_for_proc(struct Proc *p, hidx_t conn_h,
                                 struct srv_peer_info *out);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len);

// Test-support registry wipe (non-static; defined in kernel/devsrv.c;
// deliberately not in devsrv.h — no production caller).
extern void srv_registry_reset(void);

// Test-support process-table linkage (non-static; defined in kernel/proc.c;
// deliberately not in proc.h). proc_caps_by_stripes — the SYS_SRV_PEER
// live-caps scan — only sees Procs spliced into the kproc-rooted table;
// these link / unlink a bare proc_alloc'd test Proc.
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);

void test_devsrv_walk_service(void);
void test_devsrv_conn_open(void);
void test_devsrv_open_connect_byte(void);
void test_devsrv_kernel_attached_io_refused(void);
void test_devsrv_accept_immediate(void);
void test_devsrv_accept_blocks_then_wakes(void);
void test_devsrv_conn_io(void);
void test_devsrv_conn_release(void);
void test_devsrv_poster_exit_drains_backlog(void);
void test_devsrv_srv_peer_identity(void);
void test_devsrv_srv_peer_dead_peer(void);
void test_devsrv_srv_peer_gate(void);
void test_devsrv_srv_peer_bad_args(void);

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

// A test Proc spliced into the process table so proc_caps_by_stripes
// (the SYS_SRV_PEER live-caps scan) can find it — a bare proc_alloc'd
// Proc is invisible to the kproc-rooted table walk.
static struct Proc *make_linked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_test_link(p);
    return p;
}

// Tear down a make_linked_test_proc Proc: unlink from the table first so
// no dangling pointer remains, then free. proc_test_unlink is idempotent,
// so a test that already unlinked `p` may still call this.
static void drop_linked_test_proc(struct Proc *p) {
    if (!p) return;
    proc_test_unlink(p);
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

    // A clone (nname == 0) → the supplied Spoor becomes a FRESH /srv root
    // instance over the SAME registry (stalk-3a: the cross_mounts clone-walk;
    // the clone takes its own registry ref, dropped at devsrv_close). Pre-
    // stalk-3a the root carried no aux; now it names its SrvRegistry.
    struct Spoor *nc4 = spoor_clone(root);
    TEST_ASSERT(nc4 != NULL, "spoor_clone");
    struct Walkqid *wc = devsrv.walk(root, nc4, NULL, 0);
    TEST_ASSERT(wc != NULL, "clone walk (nname=0) succeeds");
    TEST_EXPECT_EQ(wc->nqid, 0, "clone walks zero components");
    TEST_ASSERT(nc4->aux == root->aux,
        "the cloned root names the same registry as its source");
    TEST_ASSERT(*(const u64 *)nc4->aux == SRV_REGISTRY_MAGIC,
        "the cloned root's aux is the SrvRegistry (SRV_REGISTRY_MAGIC)");
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
// devsrv.open_connect_byte — the stalk-3b-β open=connect path (byte-mode).
//
// devsrv_open_connect on a byte-mode service-ref Spoor mints a SrvConn, enqueues
// it on the accept backlog, and returns a CLIENT-direction byte-conn Spoor
// (CSRVCLIENT). The endpoint is a KOBJ_SPOOR and is non-dup-able (the dc='s'
// guard). The 9p-mode path (corvus) is E2E-proven once the clients migrate (3b-β-C)
// since it drives a Tversion/Tattach handshake against a live server.
// ---------------------------------------------------------------------------

void test_devsrv_open_connect_byte(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    TEST_ASSERT(sys_post_service_byte_for_proc(server, "stratum-fs", 10) >= 0,
        "post byte-mode \"stratum-fs\"");
    struct SrvService *svc = srv_lookup("stratum-fs", 10);
    TEST_ASSERT(svc != NULL, "the byte service is registered");

    // Walk /srv -> a service-ref Spoor (devsrv_walk's product).
    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv root");
    struct Spoor *sref = spoor_clone(root);
    TEST_ASSERT(sref != NULL, "spoor_clone for the service-ref");
    const char *names[1] = { "stratum-fs" };
    struct Walkqid *w = devsrv.walk(root, sref, names, 1);
    TEST_ASSERT(w != NULL && w->spoor == sref, "walk /srv/stratum-fs");
    walkqid_free(w);

    // A client opens it. Byte-mode -> a CLIENT-direction byte-conn Spoor. The
    // service-ref Spoor is NOT consumed (stalk clunks the spent quarry); the test
    // clunks it below, mirroring the resolver.
    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = devsrv_open_connect(client, sref, /*omode ORDWR*/ 2);
    TEST_ASSERT(cs != NULL, "devsrv_open_connect -> a conn Spoor");
    TEST_ASSERT(cs != sref, "the returned endpoint is a NEW Spoor (open-returns-new)");
    TEST_EXPECT_EQ((int)cs->dc, (int)'s', "the conn Spoor is a devsrv Spoor");
    TEST_ASSERT((cs->flag & CSRVCLIENT) != 0, "the conn Spoor is the CLIENT endpoint");
    TEST_ASSERT((cs->flag & COPEN) != 0, "the conn Spoor is opened");
    struct SrvConn *cn = devsrv_conn_of(cs);
    TEST_ASSERT(cn != NULL, "the conn Spoor names a SrvConn");
    TEST_ASSERT(srvconn_is_live(cn), "the SrvConn is live");
    TEST_EXPECT_EQ(srv_backlog_depth(svc), 1, "the connection is enqueued for accept");
    spoor_clunk(sref);                 // the spent quarry (stalk would clunk it)

    // The connection endpoint is non-dup-able: install it as a KOBJ_SPOOR handle
    // and assert handle_dup refuses it (the dc='s' NoSrvSpoorDup guard).
    hidx_t ch = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(ch >= 0, "install the conn Spoor as a KOBJ_SPOOR handle");
    TEST_EXPECT_EQ(handle_dup(client, ch, RIGHT_READ), -1,
        "dup of a devsrv conn Spoor (dc='s') -> -1 (NoSrvSpoorDup)");
    handle_close(client, ch);          // releases cs (devsrv_close: not kernel-attached -> teardown + unref)

    // A connect against a TOMBSTONED service mints no service-ref (walk fails),
    // so open=connect is unreachable; the poster-exit also drained the backlog.
    srv_proc_exit_notify(server);
    struct Spoor *sref2 = spoor_clone(root);
    TEST_ASSERT(sref2 != NULL, "spoor_clone");
    TEST_ASSERT(devsrv.walk(root, sref2, names, 1) == NULL,
        "walk of a tombstoned service -> NULL (no service-ref minted)");
    spoor_clunk(sref2);

    spoor_clunk(root);
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(server);
}

// ---------------------------------------------------------------------------
// devsrv.kernel_attached_io_refused — stalk-3b-E F1 regression.
//
// After SYS_ATTACH_9P_SRV wraps a byte-conn endpoint (srvconn_set_kernel_
// attached), the c2s/s2c rings are load-bearing for the kernel 9P client.
// A direct read/write on the still-held CSRVCLIENT conn Spoor must be
// REFUSED -- otherwise it drains/interleaves bytes meant for that client
// and corrupts the 9P wire (the same hazard the KOBJ_SRV r/w arms guard;
// the endpoint moved KObj_Srv -> KOBJ_SPOOR in C1 so the I/O guard had to
// follow into devsrv_read/write). Pre-fix the kernel-attached read would
// drain the staged s2c bytes (return 5); post-fix the guard returns -1.
// ---------------------------------------------------------------------------

void test_devsrv_kernel_attached_io_refused(void) {
    srv_registry_reset();

    struct Proc *server = make_marked_test_proc();
    TEST_ASSERT(server != NULL, "server proc");
    TEST_ASSERT(sys_post_service_byte_for_proc(server, "stratum-fs", 10) >= 0,
        "post byte-mode \"stratum-fs\"");

    struct Spoor *root = devsrv.attach(NULL);
    TEST_ASSERT(root != NULL, "devsrv root");
    struct Spoor *sref = spoor_clone(root);
    TEST_ASSERT(sref != NULL, "spoor_clone for the service-ref");
    const char *names[1] = { "stratum-fs" };
    struct Walkqid *w = devsrv.walk(root, sref, names, 1);
    TEST_ASSERT(w != NULL && w->spoor == sref, "walk /srv/stratum-fs");
    walkqid_free(w);

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = devsrv_open_connect(client, sref, /*omode ORDWR*/ 2);
    TEST_ASSERT(cs != NULL, "devsrv_open_connect -> a conn Spoor");
    TEST_ASSERT((cs->flag & CSRVCLIENT) != 0, "the conn Spoor is the CLIENT endpoint");
    struct SrvConn *cn = devsrv_conn_of(cs);
    TEST_ASSERT(cn != NULL, "the conn Spoor names a SrvConn");
    spoor_clunk(sref);                 // the spent quarry (stalk would clunk it)

    // Control: BEFORE kernel-attach the CSRVCLIENT I/O path is live. The
    // server stages bytes on s2c; the client read drains them; a client
    // write fills c2s.
    u8 pong[5] = { 'P', 'O', 'N', 'G', '\n' };
    TEST_EXPECT_EQ(srvconn_server_send(cn, pong, 5), 5L,
        "server stages 5 bytes on s2c");
    u8 rbuf[16];
    TEST_EXPECT_EQ(cs->dev->read(cs, rbuf, 16, 0), 5L,
        "pre-attach: client read drains s2c (path live)");
    u8 hi[3] = { 'H', 'I', '\n' };
    TEST_EXPECT_EQ(cs->dev->write(cs, hi, 3, 0), 3L,
        "pre-attach: client write fills c2s (path live)");

    // stalk-3b-E F1: after kernel-attach, direct I/O on the endpoint is
    // refused. Stage fresh s2c bytes so a pre-fix read would return 5
    // (not block on an empty ring) -- the guard must beat the drain.
    srvconn_set_kernel_attached(cn);
    TEST_EXPECT_EQ(srvconn_server_send(cn, pong, 5), 5L,
        "server stages 5 more bytes on s2c");
    TEST_EXPECT_EQ(cs->dev->read(cs, rbuf, 16, 0), -1L,
        "kernel-attached: client read REFUSED (F1)");
    TEST_EXPECT_EQ(cs->dev->write(cs, hi, 3, 0), -1L,
        "kernel-attached: client write REFUSED (F1)");

    // Release the conn Spoor. devsrv_close honors kernel_attached (skip
    // teardown, just unref the Spoor's create-ref); the backlog ref drops
    // at srv_registry_reset, freeing the SrvConn.
    spoor_clunk(cs);
    spoor_clunk(root);
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(server);
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

// ---------------------------------------------------------------------------
// devsrv.srv_peer_identity — SYS_SRV_PEER returns the kernel-stamped peer
// identity; the caps read is live.
// ---------------------------------------------------------------------------

void test_devsrv_srv_peer_identity(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    // A console-attached client with a known capability set, spliced into
    // the process table so the live-caps scan can find it.
    struct Proc *client = make_linked_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    proc_mark_console_attached(client);
    client->caps = CAP_CSPRNG_READ;

    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // corvus reads the connection's kernel-stamped peer identity.
    struct srv_peer_info info = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &info), 0,
        "SYS_SRV_PEER succeeds for the service poster");
    TEST_EXPECT_EQ(info.stripes, proc_stripes(client),
        "the peer stripes is the client's kernel identity");
    TEST_EXPECT_EQ((u64)info.caps, (u64)CAP_CSPRNG_READ,
        "the peer caps is the client's live capability set");
    TEST_EXPECT_EQ(info.console, 1u, "the peer console bit is set");
    TEST_EXPECT_EQ(info.alive, 1u, "an ALIVE peer reports alive");

    // The caps read is LIVE — a mutation of the client's caps is visible
    // on the next call, never a stale capture on the connection.
    client->caps = CAP_CSPRNG_READ | CAP_LOCK_PAGES;
    struct srv_peer_info info2 = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &info2), 0,
        "SYS_SRV_PEER succeeds on the re-query");
    TEST_EXPECT_EQ((u64)info2.caps, (u64)(CAP_CSPRNG_READ | CAP_LOCK_PAGES),
        "the re-query reflects the client's mutated caps (a live read)");

    handle_close(corvus, (hidx_t)conn_h);
    srv_registry_reset();
    drop_linked_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.srv_peer_dead_peer — the dead-Proc guard: a zombie / reaped peer
// fail-closes caps + alive while the immutable identity survives.
// ---------------------------------------------------------------------------

void test_devsrv_srv_peer_dead_peer(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_linked_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    client->caps = CAP_CSPRNG_READ;
    u64 client_stripes = proc_stripes(client);

    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // Baseline: the peer is ALIVE.
    struct srv_peer_info info = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &info), 0,
        "SYS_SRV_PEER succeeds");
    TEST_EXPECT_EQ(info.alive, 1u, "the ALIVE peer reports alive");

    // A ZOMBIE peer — the dead-Proc guard fail-closes caps + alive, but
    // the immutable identity (captured at mint) still reads truthfully.
    client->state = PROC_STATE_ZOMBIE;
    struct srv_peer_info zinfo = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &zinfo), 0,
        "SYS_SRV_PEER still returns 0 for a dead peer");
    TEST_EXPECT_EQ(zinfo.alive, 0u, "a zombie peer reports not-alive");
    TEST_EXPECT_EQ((u64)zinfo.caps, 0ull,
        "a zombie peer's caps fail-close to 0 (never a stale snapshot)");
    TEST_EXPECT_EQ(zinfo.stripes, client_stripes,
        "the immutable peer stripes survives the peer's death");

    // A reaped peer — gone from the process table entirely. Same result.
    proc_test_unlink(client);
    struct srv_peer_info rinfo = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &rinfo), 0,
        "SYS_SRV_PEER returns 0 for a reaped peer");
    TEST_EXPECT_EQ(rinfo.alive, 0u, "a reaped peer reports not-alive");
    TEST_EXPECT_EQ((u64)rinfo.caps, 0ull, "a reaped peer's caps are 0");

    handle_close(corvus, (hidx_t)conn_h);
    srv_registry_reset();
    drop_linked_test_proc(client);          // proc_test_unlink is idempotent
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.srv_peer_gate — only the service poster may query a connection's
// peer; the connection endpoint must be a KObj_Spoor.
// ---------------------------------------------------------------------------

void test_devsrv_srv_peer_gate(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_linked_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    int client_h = srv_conn_open_for_proc(client, "corvus", 6);
    TEST_ASSERT(client_h >= 0, "the client connects");
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // Baseline: the poster may query.
    struct srv_peer_info info = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)conn_h, &info), 0,
        "the poster may query its connection's peer");

    // A stranger that does NOT hold the connection handle cannot query —
    // conn_h indexes the stranger's (empty) handle table.
    struct Proc *stranger = make_test_proc();
    TEST_ASSERT(stranger != NULL, "stranger proc");
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(stranger, (hidx_t)conn_h, &info),
        -1, "a Proc without the connection handle cannot query");

    // The poster gate proper: a Proc that DOES hold a handle resolving to
    // the connection endpoint — but is not the service poster — is still
    // refused. Install the same endpoint Spoor in the stranger's table;
    // a spoor_ref balances the extra handle slot's release.
    struct Spoor *endpoint =
        (struct Spoor *)handle_get(corvus, (hidx_t)conn_h)->obj;
    spoor_ref(endpoint);
    hidx_t shared = handle_alloc(stranger, KOBJ_SPOOR,
                                 RIGHT_READ | RIGHT_WRITE, endpoint);
    TEST_ASSERT(shared >= 0, "the endpoint Spoor installs in the stranger");
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(stranger, shared, &info), -1,
        "a non-poster holding the connection endpoint is still refused");
    handle_close(stranger, shared);             // drops the spoor_ref above

    // The client's own KObj_Srv connection handle is not a SYS_SRV_PEER
    // argument — the syscall requires a KObj_Spoor endpoint.
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(client, (hidx_t)client_h, &info),
        -1, "a KObj_Srv connection handle is rejected (wrong kind)");

    handle_close(corvus, (hidx_t)conn_h);
    srv_registry_reset();
    drop_test_proc(stranger);
    drop_linked_test_proc(client);
    drop_test_proc(corvus);
}

// ---------------------------------------------------------------------------
// devsrv.srv_peer_bad_args — SYS_SRV_PEER argument validation.
// ---------------------------------------------------------------------------

void test_devsrv_srv_peer_bad_args(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = sys_post_service_for_proc(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct srv_peer_info info = {0};
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(NULL, 0, &info), -1,
        "a NULL Proc → -1");
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, 0, NULL), -1,
        "a NULL out pointer → -1");
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, -1, &info), -1,
        "a negative handle index → -1");
    TEST_EXPECT_EQ(sys_srv_peer_for_proc(corvus, (hidx_t)svc_h, &info), -1,
        "the KObj_Srv service handle is not a SYS_SRV_PEER argument");

    srv_registry_reset();
    drop_test_proc(corvus);
}
