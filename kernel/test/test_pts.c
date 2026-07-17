// PTY-1c: the pts registry (PTY-DESIGN.md section 3; <thylacine/pts.h>).
//
// Six tests:
//
//   pts.mint_bind_resolve_free
//     The lifecycle over real SrvConns: mint records the master binding,
//     slave binds add rows, resolve_conn_qid answers both sides (with the
//     master/slave discriminator) and misses cleanly, free unbinds.
//     Binding refs are proven balanced by the conn refcount returning to
//     its pre-mint value.
//
//   pts.gen_guard_stale_id
//     The SA-8/F11 regression: a pts_id held across a free + re-mint of
//     the same slot fails every later op (the gen bumped at free), never
//     mis-routing to the new occupant.
//
//   pts.authority_minting_server_only
//     A second server Proc cannot SLAVE-bind or FREE another server's pts
//     (-T_E_ACCES) -- the R2-F4 anchor.
//
//   pts.binding_dedup_bounds_uniqueness
//     An identical re-bind is idempotent (no duplicate row -- proven by
//     the row bound still admitting the same number of further binds); a
//     (conn, qid) bound to one pts rejects binding to another (-T_E_EXIST,
//     resolve stays deterministic); the per-entry row bound rejects with
//     -T_E_NOMEM; a duplicate master mint rejects -T_E_EXIST.
//
//   pts.full_registry_torn_conn_gc
//     Fill all PTS_MAX slots -> the next mint fails -T_E_AGAIN; tearing
//     one entry's only conn (the dead-server signature: #841 server-
//     endpoint teardown) lets the next mint reclaim exactly that entry.
//
//   pts.syscall_gates
//     sys_pty_register_for_proc over a fabricated server-endpoint devsrv
//     conn Spoor installed in a test Proc's handle table: the
//     MAY_POST_SERVICE gate, the bad-fd / bad-op / nonzero-a3 rejects,
//     the CSRVCLIENT client-endpoint reject, a full MINT+SLAVE+FREE round
//     trip, and pts_resolve_spoor failing closed on a non-dev9p Spoor.
//
// Conn fixtures are real srvconn_create products (the registry compares +
// ref-holds them; nothing here drives bytes). Server fixtures are bare
// proc_alloc Procs (the registry reads only ->pid; the syscall test adds a
// handle table + the service flag).

#include "test.h"

#include <thylacine/devsrv.h>
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/pts.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

void test_pts_mint_bind_resolve_free(void);
void test_pts_gen_guard_stale_id(void);
void test_pts_authority_minting_server_only(void);
void test_pts_binding_dedup_bounds_uniqueness(void);
void test_pts_full_registry_torn_conn_gc(void);
void test_pts_syscall_gates(void);

extern s64 sys_pty_register_for_proc(struct Proc *p, u64 op, u64 a1, u64 a2,
                                     u64 a3);

static struct SrvConn *pts_make_conn(struct Proc *owner) {
    return srvconn_create(proc_stripes(owner), owner->pid, false, 0,
                          SRVCONN_MSIZE);
}

static void pts_drop_conn(struct SrvConn *cn) {
    if (!cn) return;
    srvconn_teardown(cn);
    srvconn_unref(cn);
}

static void pts_drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// pts.mint_bind_resolve_free
// ---------------------------------------------------------------------------

void test_pts_mint_bind_resolve_free(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    int ref0 = cn->ref;

    // Arg rejects.
    TEST_EXPECT_EQ(pts_mint(NULL, cn, 7), -T_E_INVAL, "mint NULL server");
    TEST_EXPECT_EQ(pts_mint(srv, NULL, 7), -T_E_INVAL, "mint NULL conn");
    TEST_EXPECT_EQ(pts_mint(srv, cn, 0), -T_E_INVAL,
        "master qid 0 rejected (the dev9p attach-root qid is reserved)");

    s64 id = pts_mint(srv, cn, 7);
    TEST_ASSERT(id > 0, "mint returns a positive pts_id");
    TEST_EXPECT_EQ(cn->ref, ref0 + 1, "the master binding holds one conn ref");

    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 8, (u64)id), 0, "slave bind");
    TEST_EXPECT_EQ(cn->ref, ref0 + 2, "the slave binding holds a second ref");

    bool is_master = false;
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 7, &is_master), id,
        "the master (conn, qid) resolves to the pts");
    TEST_ASSERT(is_master, "the master side reports master");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 8, &is_master), id,
        "the slave (conn, qid) resolves to the same pts");
    TEST_ASSERT(!is_master, "the slave side reports slave");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 999, NULL), -T_E_NOENT,
        "an unbound qid misses");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 0, NULL), -T_E_INVAL,
        "qid 0 never resolves");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "free");
    TEST_EXPECT_EQ(cn->ref, ref0, "free dropped both binding refs");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 7, NULL), -T_E_NOENT,
        "a freed pts no longer resolves");

    pts_drop_conn(cn);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.gen_guard_stale_id
// ---------------------------------------------------------------------------

void test_pts_gen_guard_stale_id(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    s64 old_id = pts_mint(srv, cn, 21);
    TEST_ASSERT(old_id > 0, "first mint");
    TEST_EXPECT_EQ(pts_free(srv, (u64)old_id), 0, "free");

    // The registry reuses the lowest free slot, so this re-mint occupies
    // the SAME index the freed pts held -- the exact aliasing hazard the
    // gen closes (a stale id must not reach the new occupant).
    s64 new_id = pts_mint(srv, cn, 22);
    TEST_ASSERT(new_id > 0, "re-mint after free");
    TEST_ASSERT(new_id != old_id, "the reused slot carries a NEW gen");

    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 23, (u64)old_id), -T_E_INVAL,
        "a stale id cannot bind a slave");
    TEST_EXPECT_EQ(pts_free(srv, (u64)old_id), -T_E_INVAL,
        "a stale id cannot free (the new occupant is untouched)");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 22, NULL), new_id,
        "the new occupant still resolves after the stale-id attempts");

    TEST_EXPECT_EQ(pts_free(srv, (u64)new_id), 0, "cleanup free");
    pts_drop_conn(cn);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.authority_minting_server_only
// ---------------------------------------------------------------------------

void test_pts_authority_minting_server_only(void) {
    struct Proc *srv_a = proc_alloc();
    struct Proc *srv_b = proc_alloc();
    TEST_ASSERT(srv_a != NULL && srv_b != NULL, "proc_alloc x2");
    struct SrvConn *cn = pts_make_conn(srv_a);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    s64 id = pts_mint(srv_a, cn, 31);
    TEST_ASSERT(id > 0, "A mints");

    TEST_EXPECT_EQ(pts_bind_slave(srv_b, cn, 32, (u64)id), -T_E_ACCES,
        "B cannot slave-bind A's pts");
    TEST_EXPECT_EQ(pts_free(srv_b, (u64)id), -T_E_ACCES,
        "B cannot free A's pts");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 31, NULL), id,
        "A's pts is untouched by B's attempts");

    TEST_EXPECT_EQ(pts_free(srv_a, (u64)id), 0, "A frees its own");
    pts_drop_conn(cn);
    pts_drop_proc(srv_a);
    pts_drop_proc(srv_b);
}

// ---------------------------------------------------------------------------
// pts.binding_dedup_bounds_uniqueness
// ---------------------------------------------------------------------------

void test_pts_binding_dedup_bounds_uniqueness(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    int ref0 = cn->ref;

    s64 id_a = pts_mint(srv, cn, 41);
    TEST_ASSERT(id_a > 0, "mint A");
    TEST_EXPECT_EQ(pts_mint(srv, cn, 41), -T_E_EXIST,
        "a duplicate master (conn, qid) cannot mint a second pts");

    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 42, (u64)id_a), 0, "slave row 1");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 42, (u64)id_a), 0,
        "an identical re-bind is idempotent");
    TEST_EXPECT_EQ(cn->ref, ref0 + 2,
        "the idempotent re-bind took NO extra ref (no duplicate row)");

    s64 id_b = pts_mint(srv, cn, 51);
    TEST_ASSERT(id_b > 0, "mint B");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 42, (u64)id_b), -T_E_EXIST,
        "a (conn, qid) bound to A cannot also bind to B");

    // A holds master + 1 slave; two more rows fill PTS_BINDINGS_MAX.
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 43, (u64)id_a), 0, "slave row 2");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 44, (u64)id_a), 0, "slave row 3");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 45, (u64)id_a), -T_E_NOMEM,
        "the per-entry binding rows are bounded");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id_a), 0, "free A");
    TEST_EXPECT_EQ(pts_free(srv, (u64)id_b), 0, "free B");
    TEST_EXPECT_EQ(cn->ref, ref0, "every binding ref returned");
    pts_drop_conn(cn);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.full_registry_torn_conn_gc
// ---------------------------------------------------------------------------

void test_pts_full_registry_torn_conn_gc(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    struct SrvConn *cn_dying = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL && cn_dying != NULL, "srvconn_create x2");

    // Fill the registry: one entry on the dying conn, the rest on cn.
    s64 ids[PTS_MAX];
    ids[0] = pts_mint(srv, cn_dying, 1000);
    TEST_ASSERT(ids[0] > 0, "mint on the dying conn");
    for (u32 i = 1; i < PTS_MAX; i++) {
        ids[i] = pts_mint(srv, cn, 1000 + i);
        TEST_ASSERT(ids[i] > 0, "fill mint");
    }
    TEST_EXPECT_EQ(pts_mint(srv, cn, 2000), -T_E_AGAIN,
        "a full registry with every conn live rejects");

    // The dead-server signature: the conn tears down (#841 server-endpoint
    // teardown). The next mint reclaims exactly the torn entry.
    srvconn_teardown(cn_dying);
    s64 id_new = pts_mint(srv, cn, 2000);
    TEST_ASSERT(id_new > 0, "the torn-conn entry is reclaimed for the mint");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn_dying, 1000, NULL), -T_E_NOENT,
        "the reclaimed entry's old binding is gone");
    TEST_EXPECT_EQ(pts_free(srv, (u64)ids[0]), -T_E_INVAL,
        "the reclaimed entry's old id is stale (gen bumped by the GC)");

    for (u32 i = 1; i < PTS_MAX; i++)
        TEST_EXPECT_EQ(pts_free(srv, (u64)ids[i]), 0, "cleanup free");
    TEST_EXPECT_EQ(pts_free(srv, (u64)id_new), 0, "cleanup free (reclaimed)");
    srvconn_unref(cn_dying);
    pts_drop_conn(cn);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.syscall_gates
// ---------------------------------------------------------------------------

void test_pts_syscall_gates(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    srv->handles = handle_table_alloc();
    TEST_ASSERT(srv->handles != NULL, "handle_table_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");

    // Fabricate the server-endpoint conn Spoor the accept would mint:
    // dc='s' (spoor_alloc from devsrv), aux = the SrvConn, no CSRVCLIENT.
    // The extra srvconn_ref mirrors devsrv_make_conn_spoor's adoption --
    // devsrv_close (via the handle close below) tears down + drops it.
    struct Spoor *sp_srv = spoor_alloc(&devsrv);
    TEST_ASSERT(sp_srv != NULL, "spoor_alloc (server endpoint)");
    srvconn_ref(cn);
    sp_srv->aux = cn;
    hidx_t fd_srv = handle_alloc(srv, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE,
                                 sp_srv);
    TEST_ASSERT(fd_srv >= 0, "handle_alloc (server endpoint)");

    // The client endpoint on the SAME conn: CSRVCLIENT set.
    struct Spoor *sp_cli = spoor_alloc(&devsrv);
    TEST_ASSERT(sp_cli != NULL, "spoor_alloc (client endpoint)");
    srvconn_ref(cn);
    sp_cli->aux   = cn;
    sp_cli->flag |= CSRVCLIENT;
    hidx_t fd_cli = handle_alloc(srv, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE,
                                 sp_cli);
    TEST_ASSERT(fd_cli >= 0, "handle_alloc (client endpoint)");

    // The MAY_POST_SERVICE gate precedes fd resolution on MINT.
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_MINT, (u64)fd_srv,
                                             71, 0), -T_E_ACCES,
        "MINT without the service flag rejects");
    proc_mark_may_post_service(srv);

    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, 99, (u64)fd_srv, 71, 0),
        -T_E_INVAL, "an unknown op rejects");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_MINT, 9999, 71, 0),
        -T_E_INVAL, "a bad fd rejects");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_MINT, (u64)fd_srv,
                                             71, 5), -T_E_INVAL,
        "a nonzero x3 on MINT rejects");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_MINT, (u64)fd_cli,
                                             71, 0), -T_E_INVAL,
        "a CLIENT-endpoint conn fd cannot mint");

    s64 id = sys_pty_register_for_proc(srv, PTY_REG_MINT, (u64)fd_srv, 71, 0);
    TEST_ASSERT(id > 0, "MINT via the syscall front");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_SLAVE, (u64)fd_srv,
                                             72, (u64)id), 0,
        "SLAVE via the syscall front");
    TEST_EXPECT_EQ(pts_resolve_conn_qid(cn, 72, NULL), id,
        "the syscall-bound slave resolves");

    // pts_resolve_spoor fails closed on a non-dev9p Spoor (the conn Spoor
    // itself is devsrv, dc='s' -- dev9p_client_fid rejects it).
    TEST_EXPECT_EQ(pts_resolve_spoor(sp_srv, NULL), -T_E_INVAL,
        "resolve_spoor rejects a non-dev9p Spoor");

    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_FREE, (u64)id, 1, 0),
        -T_E_INVAL, "a nonzero x2 on FREE rejects");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_FREE, (u64)id, 0, 0),
        0, "FREE via the syscall front");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_REG_FREE, (u64)id, 0, 0),
        -T_E_INVAL, "a double FREE rejects (the gen guard)");

    // handle_close runs devsrv_close on both endpoints (teardown is
    // idempotent); each drops the ref fabricated for its Spoor.
    TEST_EXPECT_EQ(handle_close(srv, fd_srv), 0, "close the server endpoint");
    TEST_EXPECT_EQ(handle_close(srv, fd_cli), 0, "close the client endpoint");
    srvconn_unref(cn);   // the create ref
    pts_drop_proc(srv);
}
