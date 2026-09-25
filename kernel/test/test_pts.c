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
#include <thylacine/dev.h>
#include <thylacine/addrspace.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/dev9p.h>
#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/errno.h>
#include <thylacine/handle.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/pts.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>   // PTY-1f: fabricated member Threads
#include <thylacine/types.h>

static void pts_test_interaction_front(void);
static void pts_test_interaction_retirement_race(void);
static void pts_test_interaction_boundaries(void);
static void pts_test_interaction_lifecycle(void);
static void pts_test_interaction_ownership(void);
static void pts_test_interaction_capacity(void);
void test_pts_mint_bind_resolve_free(void);
void test_pts_gen_guard_stale_id(void);
void test_pts_authority_minting_server_only(void);
void test_pts_binding_dedup_bounds_uniqueness(void);
void test_pts_full_registry_torn_conn_gc(void);
void test_pts_syscall_gates(void);
void test_pts_tty_acquire_matrix(void);
void test_pts_tty_set_get_fg_matrix(void);
void test_pts_tty_signal_routing(void);

extern s64 sys_pty_register_for_proc(struct Proc *p, u64 op, u64 a1, u64 a2,
                                     u64 a3);
extern s64 sys_tty_signal_for_proc(struct Proc *p, u64 pts_id, u64 sig_class);
extern s64 sys_tty_fd_op_for_proc(struct Proc *p, u64 fd_raw, u64 op_num,
                                  u64 arg);

// Test-harness hooks (the test_proc.c pattern).
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern void proc_test_link_child(struct Proc *parent, struct Proc *p);

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

static void pts_drop_linked(struct Proc *p) {
    if (!p) return;
    proc_test_unlink(p);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static bool pts_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Count queued notes named `name` in p's queue (under q->lock).
static u32 pts_note_count(struct Proc *p, const char *name) {
    struct NoteQueue *q = p->notes;
    u32 found = 0;
    spin_lock(&q->lock);
    u32 idx = q->head;
    for (u32 n = 0; n < q->count; n++) {
        if (pts_streq(q->ring[idx].name, name)) found++;
        idx = (idx + 1) % NOTE_QUEUE_DEPTH;
    }
    spin_unlock(&q->lock);
    return found;
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

    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 8, (u64)id | (1ull << 48)),
        -T_E_INVAL, "HI1 noncanonical terminal id cannot alias a live generation");
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
    pts_test_interaction_capacity();
    pts_test_interaction_retirement_race();
    pts_test_interaction_boundaries();
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
    pts_test_interaction_front();
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    TEST_ASSERT(srv->handles != NULL, "proc_alloc supplied handle table");
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

    // Interaction fronts reject malformed operands before resolving authority.
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_BIND,
        0x100000000ull, (u64)fd_cli, 0), -T_E_INVAL, "HI1 wide fd does not narrow");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_BIND,
        (u64)fd_srv, (u64)fd_cli, 1), -T_E_INVAL, "HI1 BIND unused arg refused");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_BIND,
        9999, (u64)fd_cli, 0), -T_E_BADF, "HI1 BIND missing master fd");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_BIND,
        (u64)fd_srv, (u64)fd_cli, 0), -T_E_INVAL, "HI1 BIND needs real dev9p master");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_WATCH,
        1, 1, 0), -T_E_INVAL, "HI1 WATCH unused arg refused");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTY_INTERACTION_UNBIND,
        1ull << 63, 0, 0), -T_E_INVAL, "HI1 signed-error-space binding refused");
    TEST_EXPECT_EQ(sys_pty_register_for_proc(srv, PTS_INTERACTION_RESERVE_WATCH,
        1, 0, 0), -T_E_INVAL, "HI1 private watcher operation not dispatched");

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

    // PTY-1d fronts over the same fds: the devsrv conn Spoor is not a dev9p
    // Spoor, so the (SrvConn, qid) extraction fails closed; a bad fd + a
    // zero pts_id reject at the front.
    TEST_EXPECT_EQ(sys_tty_fd_op_for_proc(srv, (u64)fd_srv, SYS_TTY_ACQUIRE, 0),
        -T_E_INVAL, "TTY_ACQUIRE on a non-dev9p fd rejects");
    TEST_EXPECT_EQ(sys_tty_fd_op_for_proc(srv, 9999, SYS_TTY_GET_FG, 0),
        -T_E_INVAL, "TTY_GET_FG on a bad fd rejects");
    TEST_EXPECT_EQ(sys_tty_signal_for_proc(srv, 0, TTY_SIG_INT), -T_E_INVAL,
        "TTY_SIGNAL on pts_id 0 rejects");

    // handle_close runs devsrv_close on both endpoints (teardown is
    // idempotent); each drops the ref fabricated for its Spoor.
    TEST_EXPECT_EQ(handle_close(srv, fd_srv), 0, "close the server endpoint");
    TEST_EXPECT_EQ(handle_close(srv, fd_cli), 0, "close the client endpoint");
    srvconn_unref(cn);   // the create ref
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.tty_acquire_matrix
// ---------------------------------------------------------------------------

void test_pts_tty_acquire_matrix(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    s64 id = pts_mint(srv, cn, 300);
    TEST_ASSERT(id > 0, "mint");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 301, (u64)id), 0, "slave bind");

    // proc_alloc defaults sid = pgid = pid: a session leader. The
    // non-leader fabricates a foreign sid.
    struct Proc *leader_a   = proc_alloc();
    struct Proc *leader_b   = proc_alloc();
    struct Proc *non_leader = proc_alloc();
    TEST_ASSERT(leader_a && leader_b && non_leader, "proc_alloc x3");
    non_leader->sid = (u32)non_leader->pid + 1;

    TEST_EXPECT_EQ(pts_tty_acquire(non_leader, cn, 301), -T_E_ACCES,
        "a non-leader cannot acquire");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_a, cn, 300), -T_E_INVAL,
        "acquisition via the MASTER side rejects");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_a, cn, 999), -T_E_NOENT,
        "an unbound qid misses");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_a, cn, 301), 0, "A acquires");
    TEST_EXPECT_EQ(pts_tty_get_fg(leader_a, cn, 301), (s64)leader_a->pgid,
        "acquisition seats fg = the leader's pgid");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_a, cn, 301), 0,
        "re-acquiring one's own is the second-open inherit (0)");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_b, cn, 301), -T_E_ACCES,
        "another session's terminal is never stolen (F7)");

    // One controlling terminal per session: A cannot take a second pts;
    // B (whose steal failed, so B still has none) can.
    s64 id2 = pts_mint(srv, cn, 310);
    TEST_ASSERT(id2 > 0, "mint 2");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 311, (u64)id2), 0, "slave bind 2");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_a, cn, 311), -T_E_ACCES,
        "a session with a controlling terminal cannot acquire a second");
    TEST_EXPECT_EQ(pts_tty_acquire(leader_b, cn, 311), 0, "B acquires pts 2");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "cleanup free 1");
    TEST_EXPECT_EQ(pts_free(srv, (u64)id2), 0, "cleanup free 2");
    pts_drop_conn(cn);
    pts_drop_proc(leader_a);
    pts_drop_proc(leader_b);
    pts_drop_proc(non_leader);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.tty_set_get_fg_matrix
// ---------------------------------------------------------------------------

void test_pts_tty_set_get_fg_matrix(void) {
    pts_test_interaction_ownership();
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    s64 id = pts_mint(srv, cn, 400);
    TEST_ASSERT(id > 0, "mint");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 401, (u64)id), 0, "slave bind");

    struct Proc *leader = proc_alloc();       // session S = leader->pid
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    struct Proc *member = proc_alloc();       // its own group, in S; LINKED so
    TEST_ASSERT(member != NULL, "proc_alloc member");
    member->sid  = (u32)leader->pid;          // the session walk finds it
    member->pgid = (u32)member->pid;
    proc_test_link(member);
    struct Proc *outsider = proc_alloc();     // its own session; linked so its
    TEST_ASSERT(outsider != NULL, "proc_alloc outsider");
    proc_test_link(outsider);                 // own group passes membership

    TEST_EXPECT_EQ(pts_tty_acquire(leader, cn, 401), 0, "leader acquires");

    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 401, 0), -T_E_INVAL,
        "pgid 0 rejects");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 401, 7777u), -T_E_ACCES,
        "a pgid with no ALIVE member in the session rejects");
    TEST_EXPECT_EQ(pts_tty_set_fg(outsider, cn, 401, (u32)outsider->pid),
        -T_E_ACCES, "a caller outside the controlling session cannot seat fg");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 401, (u32)member->pid), 0,
        "the leader seats the member's group");
    TEST_EXPECT_EQ(pts_tty_get_fg(leader, cn, 401), (s64)(u32)member->pid,
        "get reads the seated fg");
    TEST_EXPECT_EQ(pts_tty_get_fg(member, cn, 401), (s64)(u32)member->pid,
        "a controlling-session member reads via the slave side");
    TEST_EXPECT_EQ(pts_tty_get_fg(outsider, cn, 401), -T_E_ACCES,
        "an outsider's slave-side read rejects");
    TEST_EXPECT_EQ(pts_tty_get_fg(outsider, cn, 400), (s64)(u32)member->pid,
        "the MASTER side reads unconditionally (the emulator's view)");
    TEST_EXPECT_EQ(pts_tty_get_fg(leader, cn, 999), -T_E_NOENT,
        "an unbound qid misses");

    // A pts nobody controls: seating fg rejects (no controlling session).
    s64 id2 = pts_mint(srv, cn, 410);
    TEST_ASSERT(id2 > 0, "mint 2");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 411, (u64)id2), 0, "slave bind 2");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 411, (u32)member->pid),
        -T_E_ACCES, "an unowned pts rejects tcsetpgrp");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "cleanup free 1");
    TEST_EXPECT_EQ(pts_free(srv, (u64)id2), 0, "cleanup free 2");
    pts_drop_conn(cn);
    pts_drop_linked(member);
    pts_drop_linked(outsider);
    pts_drop_proc(leader);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.tty_signal_routing
// ---------------------------------------------------------------------------

void test_pts_tty_signal_routing(void) {
    struct Proc *srv       = proc_alloc();
    struct Proc *other_srv = proc_alloc();
    TEST_ASSERT(srv && other_srv, "proc_alloc x2");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    s64 id = pts_mint(srv, cn, 500);
    TEST_ASSERT(id > 0, "mint");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 501, (u64)id), 0, "slave bind");

    // The controlling session: a LINKED leader (the F13 leader post walks
    // the table) + a LINKED fg-group member in a DIFFERENT group of the
    // same session (so the leader is NOT in fg -- the dual-target case).
    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *fgm = proc_alloc();
    TEST_ASSERT(fgm != NULL, "proc_alloc fgm");
    fgm->sid  = (u32)leader->pid;
    fgm->pgid = (u32)fgm->pid;
    proc_test_link(fgm);

    TEST_EXPECT_EQ(pts_tty_acquire(leader, cn, 501), 0, "leader acquires");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 501, (u32)fgm->pid), 0,
        "fg = the member's group (the leader outside it)");

    // Gates.
    TEST_EXPECT_EQ(pts_tty_signal(other_srv, (u64)id, TTY_SIG_INT),
        -T_E_ACCES, "only the minting server signals");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, 0), -T_E_INVAL,
        "class below the range rejects");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, 99), -T_E_INVAL,
        "class above the range rejects");
    // TSTP is LIVE (PTY-1f). This fixture's fg member is THREAD-LESS, so
    // the catchability gate reads "no unmasked thread" -> the fail-safe
    // note-only disposition (nothing to stop); the stop legs live in
    // pts.tty_tstp_stop_cont_seam.
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "TSTP on a thread-less member is note-only (fail-safe)");
    TEST_EXPECT_EQ(pts_note_count(fgm, NOTE_NAME_TTY_SUSP), 1u,
        "the fg member got the susp note");
    TEST_EXPECT_EQ((int)fgm->job_stop_req, 0,
        "note-only disposition set no job stop");
    s64 scratch = pts_mint(srv, cn, 510);
    TEST_ASSERT(scratch > 0, "scratch mint");
    TEST_EXPECT_EQ(pts_free(srv, (u64)scratch), 0, "scratch free");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)scratch, TTY_SIG_INT),
        -T_E_INVAL, "a stale id rejects (the gen guard)");

    // Routing: INT/WINCH reach exactly the fg group.
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_INT), 1,
        "INT posts to the one fg member");
    TEST_EXPECT_EQ(pts_note_count(fgm, NOTE_NAME_INTERRUPT), 1u,
        "the fg member got the interrupt");
    TEST_EXPECT_EQ(pts_note_count(leader, NOTE_NAME_INTERRUPT), 0u,
        "the out-of-fg leader did NOT get the interrupt");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_WINCH), 1,
        "WINCH posts to the fg");
    TEST_EXPECT_EQ(pts_note_count(fgm, NOTE_NAME_TTY_WINCH), 1u,
        "the fg member got the winch");

    // HUP: the two POSIX carrier-loss targets (F13) -- the fg group AND the
    // controlling process (the session leader) when the leader is outside fg.
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_HUP), 2,
        "HUP reaches the fg member AND the out-of-fg leader");
    TEST_EXPECT_EQ(pts_note_count(fgm, NOTE_NAME_TTY_HUP), 1u,
        "the fg member got the hup");
    TEST_EXPECT_EQ(pts_note_count(leader, NOTE_NAME_TTY_HUP), 1u,
        "the controlling process got the hup");

    // The leader seated INTO fg: a single deduped target.
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 501, (u32)leader->pgid), 0,
        "fg = the leader's own group");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_HUP), 1,
        "HUP posts once when the leader is in fg (no double post)");
    TEST_EXPECT_EQ(pts_note_count(leader, NOTE_NAME_TTY_HUP), 2u,
        "the leader's hup arrived via the fg fan-out only");

    // A pts with no controlling session routes nowhere.
    s64 id2 = pts_mint(srv, cn, 520);
    TEST_ASSERT(id2 > 0, "mint 2");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id2, TTY_SIG_INT), 0,
        "no controlling session -> posted count 0");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "cleanup free 1");
    TEST_EXPECT_EQ(pts_free(srv, (u64)id2), 0, "cleanup free 2");
    pts_drop_conn(cn);
    pts_drop_linked(leader);
    pts_drop_linked(fgm);
    pts_drop_proc(srv);
    pts_drop_proc(other_srv);
}

// ---------------------------------------------------------------------------
// pts.tty_tstp_stop_cont_seam (PTY-1f)
// ---------------------------------------------------------------------------
// The TSTP -> job-stop -> SYS_TTY_CONT seam over a THREADED fg member: the
// uncaught default STOP consumes the signal (flag + report latch, NO queued
// note), the catchability gates (self-managing; all-threads-masked) are
// note-only, a second TSTP on a stopped member neither re-latches nor
// re-posts, and pts_tty_cont's SET_FG-shaped gates + per-member resume
// (note + flag clear + cont latch) run end-to-end. Threads are fabricated
// statics (the deliver cascade walks them; zeroed state = unlocked wait_lock
// + no rendez -> the wakes no-op), the devproc fixture pattern.

void test_pts_tty_tstp_stop_cont_seam(void);
void test_pts_tty_tstp_stop_cont_seam(void) {
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc srv");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    s64 id = pts_mint(srv, cn, 600);
    TEST_ASSERT(id > 0, "mint");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 601, (u64)id), 0, "slave bind");

    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *m = proc_alloc();          // the fg member, own group, in S
    TEST_ASSERT(m != NULL, "proc_alloc m");
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    // BSS-zeroed static (a whole-struct compound-literal assignment would
    // emit a memset the freestanding kernel does not link); the machinery
    // reads magic + note_mask + next_in_proc + rendez_blocked_on +
    // wait_lock, all zero-correct except magic.
    static struct Thread m_th;
    m_th.magic = THREAD_MAGIC;
    m_th.note_mask = 0;
    m_th.next_in_proc = NULL;
    m_th.rendez_blocked_on = NULL;
    m->threads = &m_th;                     // unmasked -> the stop can land
    // Linked UNDER the leader (the shell-parent shape): the leader -- same
    // session, another group -- ANCHORS m's group, else the TSTP fan would
    // correctly DISCARD the stop as orphaned (proc.job_stop_orphan_rule).
    proc_test_link_child(leader, m);
    struct Proc *outsider = proc_alloc();   // its own session; group linked
    TEST_ASSERT(outsider != NULL, "proc_alloc outsider");
    proc_test_link(outsider);

    TEST_EXPECT_EQ(pts_tty_acquire(leader, cn, 601), 0, "leader acquires");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 601, (u32)m->pid), 0,
        "fg = the member's group");

    // The uncaught default STOP: flag + report latch, NO note queued (the
    // default action CONSUMES the signal -- nothing pending across the stop).
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "TSTP stops the one unmasked fg member");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1, "job_stop_req set");
    TEST_ASSERT(m->stop_report_pending, "the stop latched the wait report");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_SUSP), 0u,
        "the default stop queued NO susp note");

    // A second TSTP on an already-stopped member is a POSIX discard: no
    // re-latch (a consumed report stays consumed), no note.
    m->stop_report_pending = false;
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "a second TSTP visits the member (idempotent stop)");
    TEST_ASSERT(!m->stop_report_pending,
        "the idempotent stop did NOT re-latch the report");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_SUSP), 0u,
        "the idempotent stop queued no note either");
    m->stop_report_pending = true;          // restore the latch for the cont

    // pts_tty_cont's gates (the SET_FG shape).
    TEST_EXPECT_EQ(pts_tty_cont(leader, cn, 601, 0), -T_E_INVAL,
        "cont pgid 0 rejects");
    TEST_EXPECT_EQ(pts_tty_cont(leader, cn, 601, 7777u), -T_E_ACCES,
        "cont on a group with no ALIVE session member rejects");
    TEST_EXPECT_EQ(pts_tty_cont(outsider, cn, 601, (u32)outsider->pid),
        -T_E_ACCES, "an outside-session caller cannot cont");
    TEST_EXPECT_EQ(pts_tty_cont(leader, cn, 999, (u32)m->pid), -T_E_NOENT,
        "cont via an unbound qid misses");

    // The resume: note + flag clear + the cont report superseding the stop.
    TEST_EXPECT_EQ(pts_tty_cont(leader, cn, 601, (u32)m->pid), 1,
        "SYS_TTY_CONT resumes the member's group");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0, "job_stop_req cleared");
    TEST_ASSERT(m->cont_report_pending, "the cont latched the wait report");
    TEST_ASSERT(!m->stop_report_pending,
        "the cont superseded the unreported stop");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_CONT), 1u,
        "the member got the tty:cont note");

    // The catchability gates: self-managing -> note-only; all-masked ->
    // note-only (deferred).
    proc_mark_self_managing_notes(m);
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "TSTP on a self-managing member is caught");
    TEST_EXPECT_EQ((int)m->job_stop_req, 0, "caught: no stop");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_SUSP), 1u,
        "caught: the susp note was delivered to the queue");

    struct Proc *m2 = proc_alloc();         // the all-masked member
    TEST_ASSERT(m2 != NULL, "proc_alloc m2");
    m2->sid  = (u32)leader->pid;
    m2->pgid = (u32)m2->pid;
    static struct Thread m2_th;             // BSS-zeroed (see m_th)
    m2_th.magic = THREAD_MAGIC;
    m2_th.note_mask = (1ull << NOTE_BIT_TTY);
    m2_th.next_in_proc = NULL;
    m2_th.rendez_blocked_on = NULL;
    m2->threads = &m2_th;
    proc_test_link(m2);
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 601, (u32)m2->pid), 0,
        "fg = the masked member's group");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "TSTP on an all-masked member defers");
    TEST_EXPECT_EQ((int)m2->job_stop_req, 0, "all-masked: no stop");
    TEST_EXPECT_EQ(pts_note_count(m2, NOTE_NAME_TTY_SUSP), 1u,
        "all-masked: the note queued (deferred delivery)");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "cleanup free");
    m->threads  = NULL;                     // the statics outlive proc_free
    m2->threads = NULL;
    pts_drop_conn(cn);
    pts_drop_linked(m);                     // unlinks from leader (its parent)
    pts_drop_linked(m2);
    pts_drop_linked(outsider);
    pts_drop_linked(leader);
    pts_drop_proc(srv);
}

// ---------------------------------------------------------------------------
// pts.teardown_hup_cont (PTY-1f, F8)
// ---------------------------------------------------------------------------
// The pts-teardown carrier-loss fan: freeing a controlled pts whose fg group
// holds a job-stopped member delivers tty:hup (dual target -- the fg member
// AND the out-of-fg session leader) + tty:cont, resumes the stop, and arms
// the uncaught-hup terminate latch. The GC arm shares pts_teardown_fan with
// FREE (one staging path), so the explicit-free proof covers the shape.

void test_pts_teardown_hup_cont(void);
void test_pts_teardown_hup_cont(void) {
    pts_test_interaction_lifecycle();
    struct Proc *srv = proc_alloc();
    TEST_ASSERT(srv != NULL, "proc_alloc srv");
    struct SrvConn *cn = pts_make_conn(srv);
    TEST_ASSERT(cn != NULL, "srvconn_create");
    s64 id = pts_mint(srv, cn, 700);
    TEST_ASSERT(id > 0, "mint");
    TEST_EXPECT_EQ(pts_bind_slave(srv, cn, 701, (u64)id), 0, "slave bind");

    struct Proc *leader = proc_alloc();
    TEST_ASSERT(leader != NULL, "proc_alloc leader");
    proc_test_link(leader);
    struct Proc *m = proc_alloc();
    TEST_ASSERT(m != NULL, "proc_alloc m");
    m->sid  = (u32)leader->pid;
    m->pgid = (u32)m->pid;
    static struct Thread f8_th;             // BSS-zeroed (see the seam test)
    f8_th.magic = THREAD_MAGIC;
    f8_th.note_mask = 0;
    f8_th.next_in_proc = NULL;
    f8_th.rendez_blocked_on = NULL;
    m->threads = &f8_th;
    proc_test_link_child(leader, m);        // the leader anchors m's group

    TEST_EXPECT_EQ(pts_tty_acquire(leader, cn, 701), 0, "leader acquires");
    TEST_EXPECT_EQ(pts_tty_set_fg(leader, cn, 701, (u32)m->pid), 0,
        "fg = the member's group (the leader outside it)");
    TEST_EXPECT_EQ(pts_tty_signal(srv, (u64)id, TTY_SIG_TSTP), 1,
        "stop the fg member");
    TEST_EXPECT_EQ((int)m->job_stop_req, 1, "stopped");

    TEST_EXPECT_EQ(pts_free(srv, (u64)id), 0, "FREE the controlled pts");

    TEST_EXPECT_EQ((int)m->job_stop_req, 0,
        "the teardown fan resumed the stopped fg member (F8)");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_HUP), 1u,
        "the fg member got the carrier-loss hup");
    TEST_EXPECT_EQ(pts_note_count(m, NOTE_NAME_TTY_CONT), 1u,
        "...and the cont");
    TEST_EXPECT_EQ(pts_note_count(leader, NOTE_NAME_TTY_HUP), 1u,
        "the out-of-fg controlling process got the hup (F13 dual target)");
    TEST_ASSERT((__atomic_load_n(&m->proc_flags, __ATOMIC_ACQUIRE) &
                 PROC_FLAG_TTY_TERMINATE_PENDING) != 0,
        "the uncaught hup armed the terminate latch on the resumed member");

    m->threads = NULL;
    pts_drop_conn(cn);
    pts_drop_linked(m);                     // unlinks from leader (its parent)
    pts_drop_linked(leader);
    pts_drop_proc(srv);
}

// HI-1 regressions are called from the existing pts test entries so the
// separately preserved authority draft in test.c is never staged or rewritten.
static long pti_test_read(struct Proc *p, struct Spoor *sp, void *out, long n) {
    struct Thread *t = current_thread();
    struct Proc *saved = t->proc;
    t->proc = p;
    long result = sp->dev->read(sp, out, n, 0);
    t->proc = saved;
    return result;
}

static short pti_test_poll(struct Proc *p, struct Spoor *sp, struct poll_waiter *pw) {
    struct Thread *t = current_thread();
    struct Proc *saved = t->proc;
    t->proc = p;
    short result = sp->dev->poll(sp, POLLIN, pw);
    t->proc = saved;
    return result;
}

static void pts_test_interaction_ownership(void) {
    struct Proc *host = proc_alloc(), *observer = proc_alloc(), *subject = proc_alloc();
    struct Proc *stranger = proc_alloc();
    TEST_ASSERT(host && observer && subject && stranger, "HI1 fixture processes");
    proc_test_link(host); proc_test_link(observer); proc_test_link(subject); proc_test_link(stranger);
    subject->principal_id = host->principal_id = 1001;
    stranger->principal_id = 1002;
    struct SrvConn *cn = pts_make_conn(host);
    TEST_ASSERT(cn, "HI1 fixture transport");
    s64 id = pts_mint(host, cn, 900);
    TEST_ASSERT(id > 0, "HI1 fixture terminal");
    TEST_EXPECT_EQ(pts_bind_slave(host, cn, 901, (u64)id), 0, "HI1 fixture slave");
    TEST_EXPECT_EQ(pts_tty_acquire(subject, cn, 901), 0, "HI1 acquire");
    struct pts_interaction_call c = { .pts_id = (u64)id, .observer_stripes = proc_stripes(observer) };
    TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_BIND, &c), -T_E_ACCES, "HI1 unsealed host refused");
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    s64 bid = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
    TEST_ASSERT(bid > 0, "HI1 sealed bind");
    TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_BIND, &c), -T_E_BUSY, "HI1 no binding theft");
    c.binding_id = (u64)bid;
    TEST_EXPECT_EQ(proc_pts_interaction(stranger, PTY_INTERACTION_STATE, &c), -T_E_ACCES, "HI1 foreign role denied");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), 0, "HI1 observer state");
    TEST_EXPECT_EQ(c.state.binder_pid, (u32)host->pid, "HI1 actual binder PID");
    TEST_EXPECT_EQ(c.state.flags, PTY_INTERACTION_LIVE, "HI1 initially unacknowledged");
    c.request = (struct t_pty_interaction_check){ .version = 1, .size = 24,
        .expected_epoch = c.state.foreground_epoch, .subject_stripes = proc_stripes(subject) };
    TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_ACK, &c), -T_E_ACCES, "HI1 host cannot nominate");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), -T_E_AGAIN, "HI1 no implicit acknowledgement");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 nominate foreground");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), 0, "HI1 fresh admission");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), 0, "HI1 acknowledged state");
    u64 revision = c.state.revision;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 duplicate ACK");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), 0, "HI1 duplicate state");
    TEST_EXPECT_EQ(c.state.revision, revision, "HI1 duplicate ACK is idempotent");

    s64 error;
    struct Spoor *watch = pts_interaction_watch(observer, (u64)bid, &error);
    TEST_ASSERT(watch && error == 0, "HI1 observer watch");
    TEST_ASSERT(!pts_interaction_watch(observer, (u64)bid, &error) && error == -T_E_BUSY, "HI1 one watcher per role");
    struct t_pty_interaction_state out;
    TEST_EXPECT_EQ(pti_test_read(stranger, watch, &out, sizeof(out)), -T_E_ACCES, "HI1 inherited watch cannot read");
    TEST_EXPECT_EQ(pti_test_poll(stranger, watch, NULL), POLLERR, "HI1 inherited watch cannot poll");
    TEST_EXPECT_EQ(pti_test_read(observer, watch, &out, sizeof(out)-1), -T_E_INVAL, "HI1 short read preserves cursor");
    TEST_EXPECT_EQ(pti_test_read(observer, watch, &out, sizeof(out)), 80, "HI1 initial snapshot unread");
    TEST_EXPECT_EQ(pti_test_read(observer, watch, &out, sizeof(out)), -T_E_AGAIN, "HI1 no new revision");
    struct Spoor *clone = spoor_clone(watch);
    TEST_ASSERT(clone, "HI1 navigation clone fixture");
    TEST_EXPECT_EQ(pti_test_read(observer, clone, &out, sizeof(out)), -T_E_BADF, "HI1 unopened clone cannot read");
    spoor_clunk(clone);

    struct Rendez rendez; rendez_init(&rendez);
    struct poll_waiter waiter; poll_waiter_init(&waiter, &rendez);
    TEST_EXPECT_EQ(pti_test_poll(observer, watch, &waiter), 0, "HI1 register then observe");
    // A redundant SET_FG is still a handover barrier. This bypasses every
    // shell/host notification, the counterexample that required the kernel seam.
    TEST_EXPECT_EQ(pts_tty_set_fg(subject, cn, 901, subject->pgid), 0, "HI1 direct redundant SET_FG");
    bool woken = waiter.ready;
    poll_waiter_list_unregister(&waiter);
    TEST_ASSERT(woken, "HI1 direct SET_FG wakes observer");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), -T_E_AGAIN, "HI1 stale admission after direct SET_FG");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), -T_E_AGAIN, "HI1 delayed ACK refused");
    TEST_EXPECT_EQ(pti_test_read(observer, watch, &out, sizeof(out)), 80, "HI1 changed snapshot");
    TEST_EXPECT_EQ(out.subject_stripes, 0, "HI1 controller cleared");
    c.request.expected_epoch = out.foreground_epoch;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 fresh renomination");
    // A live member can be nominated independently of the group leader.
    struct Proc *member = proc_alloc();
    TEST_ASSERT(member, "HI1 member fixture");
    member->principal_id = host->principal_id;
    member->sid = subject->sid; member->pgid = subject->pgid;
    proc_test_link(member);
    c.request.subject_stripes = proc_stripes(member);
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 pipeline member nominated");
    TEST_EXPECT_EQ(proc_setpgid(member, 0, 0), 0, "HI1 real setpgid publication");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), -T_E_AGAIN, "HI1 group change rejects old ACK");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), 0, "HI1 post-group state");
    TEST_ASSERT(c.state.foreground_epoch > c.request.expected_epoch && c.state.subject_stripes == 0,
                "HI1 group hook advances epoch and clears nomination");
    TEST_EXPECT_EQ(proc_setpgid(member, 0, (int)subject->pgid), 0, "HI1 restore member group");
    c.request.expected_epoch = c.state.foreground_epoch;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 member nomination before setsid");
    TEST_ASSERT(proc_setsid(member) > 0, "HI1 real setsid publication");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), -T_E_AGAIN, "HI1 setsid invalidates admission");
    pts_drop_linked(member);
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), 0, "HI1 latest epoch");
    c.request.expected_epoch = c.state.foreground_epoch;
    c.request.subject_stripes = proc_stripes(subject);
    c.request.version = 2;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), -T_E_INVAL, "HI1 unknown request version");
    c.request.version = 1; c.request.size = 23;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), -T_E_INVAL, "HI1 exact request size");
    c.request.size = 24;
    stranger->sid = subject->sid; stranger->pgid = subject->pgid;
    c.request.subject_stripes = proc_stripes(stranger);
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), -T_E_ACCES, "HI1 foreground principal mismatch");
    c.request.subject_stripes = 0;
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_ACK, &c), 0, "HI1 APP acknowledgement");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), -T_E_INVAL, "HI1 APP grants no admission");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_UNBIND, &c), 0, "HI1 observer revoke");
    TEST_EXPECT_EQ(pti_test_poll(observer, watch, NULL), POLLHUP, "HI1 retired watcher HUP");
    TEST_EXPECT_EQ(pti_test_read(observer, watch, &out, sizeof(out)), 0, "HI1 retired watcher EOF");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_CHECK, &c), -T_E_NOENT, "HI1 retired admission denied");
    spoor_clunk(watch);
    TEST_EXPECT_EQ(pts_free(host, (u64)id), 0, "HI1 free terminal");
    pts_drop_conn(cn);
    pts_drop_linked(stranger); pts_drop_linked(subject); pts_drop_linked(observer); pts_drop_linked(host);
}

static void pts_test_interaction_capacity(void) {
    struct Proc *host = proc_alloc(), *observer = proc_alloc();
    TEST_ASSERT(host && observer, "HI1 capacity procs");
    proc_test_link(host); proc_test_link(observer);
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    struct SrvConn *cn = pts_make_conn(host);
    TEST_ASSERT(cn, "HI1 capacity conn");
    s64 id = pts_mint(host, cn, 910);
    TEST_ASSERT(id > 0, "HI1 capacity terminal");
    struct Spoor *held[PTS_MAX];
    struct pts_interaction_call c = { .pts_id = (u64)id, .observer_stripes = proc_stripes(observer) };
    u64 previous = 0;
    for (u32 i = 0; i < PTS_MAX; ++i) {
        s64 bid = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
        TEST_ASSERT(bid > 0 && (u64)bid > previous, "HI1 unique increasing IDs");
        c.binding_id = previous = (u64)bid;
        s64 error;
        held[i] = pts_interaction_watch(observer, (u64)bid, &error);
        TEST_ASSERT(held[i], "HI1 bounded retained watch");
        TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c), 0, "HI1 retain retired slot");
    }
    TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_BIND, &c), -T_E_NOSPC, "HI1 retired watchers bound pool");
    spoor_clunk(held[0]);
    s64 reused = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
    TEST_ASSERT(reused > 0 && (u64)reused > previous, "HI1 released slot new incarnation");
    // The old locator cannot alias the recycled slot.
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), -T_E_NOENT, "HI1 old binding stays retired");
    c.binding_id = (u64)reused;
    TEST_EXPECT_EQ(pts_free(host, (u64)id), 0, "HI1 free retires live binding");
    TEST_EXPECT_EQ(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c), -T_E_NOENT, "HI1 terminal free invalidates binding");
    for (u32 i = 1; i < PTS_MAX; ++i) spoor_clunk(held[i]);
    pts_drop_conn(cn); pts_drop_linked(observer); pts_drop_linked(host);
}

struct pti_lifecycle_fixture {
    u64 pts_id;
    struct Proc *observer;
    struct Spoor *watch;
    s64 binding_id, after_exec;
    bool do_exec;
};

static void pti_lifecycle_child(void *arg) {
    struct pti_lifecycle_fixture *f = arg;
    struct Proc *self = current_thread()->proc;
    proc_seal(self, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    struct pts_interaction_call c = { .pts_id = f->pts_id,
        .observer_stripes = proc_stripes(f->observer) };
    f->binding_id = proc_pts_interaction(self, PTY_INTERACTION_BIND, &c);
    if (f->binding_id <= 0) exits("HI1 bind failed");
    c.binding_id = (u64)f->binding_id;
    s64 error;
    // Kernel fixture opens the observer role on its behalf; no native API can
    // name another Proc this way. The read below runs as the actual observer.
    f->watch = pts_interaction_watch(f->observer, c.binding_id, &error);
    if (!f->watch) exits("HI1 watch failed");
    if (f->do_exec) {
        struct AddrSpace *next = addrspace_alloc(1024);
        if (!next) exits("HI1 image allocation failed");
        proc_exec_replace(self, next, PHENO_NATIVE);
        // Measured BEFORE death: death cannot mask a missing exec hook.
        f->after_exec = proc_pts_interaction(self, PTY_INTERACTION_STATE, &c);
    }
    exits("ok");
}

static void pts_test_interaction_lifecycle(void) {
    struct Proc *observer = current_thread()->proc;
    struct Proc *host = proc_alloc();
    TEST_ASSERT(host, "HI1 lifecycle host");
    proc_test_link(host);
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    struct SrvConn *cn = pts_make_conn(host);
    TEST_ASSERT(cn, "HI1 lifecycle connection");
    s64 id = pts_mint(host, cn, 920);
    TEST_ASSERT(id > 0, "HI1 lifecycle terminal");
    for (unsigned mode = 0; mode < 2; ++mode) {
        struct pti_lifecycle_fixture f = { .pts_id = (u64)id, .observer = observer,
            .do_exec = mode != 0, .after_exec = 123 };
        int child = rfork(RFPROC, pti_lifecycle_child, &f);
        TEST_ASSERT(child > 0, "HI1 lifecycle rfork");
        int status = -1;
        TEST_EXPECT_EQ(wait_pid_for(child, 0, &status), child, "HI1 lifecycle reap");
        TEST_EXPECT_EQ(status, 0, "HI1 lifecycle child completed");
        TEST_ASSERT(f.watch, "HI1 lifecycle watch retained");
        if (mode) TEST_EXPECT_EQ(f.after_exec, -T_E_NOENT, "HI1 exec retires before child exits");
        struct t_pty_interaction_state state;
        TEST_EXPECT_EQ(pti_test_poll(observer, f.watch, NULL), POLLHUP, "HI1 actual lifecycle HUP");
        TEST_EXPECT_EQ(pti_test_read(observer, f.watch, &state, sizeof(state)), 0, "HI1 actual lifecycle EOF");
        spoor_clunk(f.watch);
        struct pts_interaction_call c = { .pts_id = (u64)id, .observer_stripes = proc_stripes(observer) };
        s64 next = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
        TEST_ASSERT(next > f.binding_id, "HI1 death releases live terminal binding");
        c.binding_id = (u64)next;
        TEST_EXPECT_EQ(proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c), 0, "HI1 lifecycle cleanup binding");
    }
    TEST_EXPECT_EQ(pts_free(host, (u64)id), 0, "HI1 lifecycle terminal cleanup");
    pts_drop_conn(cn); pts_drop_linked(host);
}

// Native front over real dev9p/SrvConn objects. Only the qid assignment is a
// fixture: version/attach consume frozen wire replies; no walk is performed.
// In particular, no caller supplies the
// observer's stripes to sys_pty_register_for_proc.
static struct p9_client pti_front_clients[2];
static u8 pti_front_rx[2][64];
static void pts_test_interaction_front(void) {
    const char *failure = NULL;
    struct Proc *host = proc_alloc(), *observer = kproc();
    struct SrvConn *conns[2] = { NULL, NULL };
    struct p9_srvconn_transport transports[2] = {0};
    bool linked = false, initialized[2] = {false, false};
    struct Spoor *roots[2] = {NULL, NULL};
    s64 terminal = -1, binding = -1;
    hidx_t master_fd = -1, root_fd = -1, client_fd = -1, server_fd = -1;
#define PTI_FRONT_CHECK(condition, message) \
    do { if (!(condition)) { failure = message; goto cleanup; } } while (0)
    PTI_FRONT_CHECK(host && observer, "HI1 front processes allocated");
    proc_test_link(host); linked = true;
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    for (unsigned i = 0; i < 2; ++i) {
        struct Proc *poster = i ? observer : host;
        conns[i] = srvconn_create(proc_stripes(host), host->pid, false,
                                  proc_stripes(poster), SRVCONN_MSIZE);
        PTI_FRONT_CHECK(conns[i], "HI1 front transport allocated");
        srvconn_set_byte_mode(conns[i]);
        PTI_FRONT_CHECK(p9_srvconn_transport_init(&transports[i], conns[i]) == 0,
                        "HI1 front adapter initialized");
        PTI_FRONT_CHECK(p9_client_init(&pti_front_clients[i], 0, SRVCONN_MSIZE,
            p9_srvconn_transport_ops(&transports[i]), pti_front_rx[i], 64) == 0,
            "HI1 front client initialized");
        initialized[i] = true;
        // Frozen 9P2000.L Rversion(msize 8192) + Rattach(tag 0, directory qid).
        // The actual client emits and consumes the handshake through SrvConn.
        const u8 replies[] = {
            21,0,0,0,P9_RVERSION,255,255,0,32,0,0,8,0,'9','P','2','0','0','0','.','L',
            20,0,0,0,P9_RATTACH,0,0,P9_QTDIR,0,0,0,0,42,0,0,0,0,0,0,0,
        };
        PTI_FRONT_CHECK(srvconn_server_send(conns[i], replies, sizeof(replies)) == sizeof(replies),
                        "HI1 front handshake replies staged");
        PTI_FRONT_CHECK(p9_client_handshake(&pti_front_clients[i], NULL, 0, NULL, 0, 1001) == 0,
                        "HI1 front real transport handshake");
        roots[i] = dev9p_attach_client(&pti_front_clients[i], 0);
        PTI_FRONT_CHECK(roots[i], "HI1 front root attached");
    }
    roots[0]->qid.path = 940;
    roots[0]->qid.type = 0;
    terminal = pts_mint(host, conns[0], 940);
    PTI_FRONT_CHECK(terminal > 0, "HI1 front terminal minted");
    PTI_FRONT_CHECK(pts_bind_slave(host, conns[0], 941, (u64)terminal) == 0,
                    "HI1 front slave registered");
    spoor_ref(roots[0]);
    master_fd = handle_alloc(host, KOBJ_SPOOR, RIGHT_READ, roots[0]);
    if (master_fd < 0) spoor_clunk(roots[0]);
    spoor_ref(roots[1]);
    root_fd = handle_alloc(host, KOBJ_SPOOR, RIGHT_READ, roots[1]);
    if (root_fd < 0) spoor_clunk(roots[1]);
    PTI_FRONT_CHECK(master_fd >= 0 && root_fd >= 0, "HI1 front root handles");
    for (unsigned i = 0; i < 2; ++i) {
        struct Spoor *sp = spoor_alloc(&devsrv);
        PTI_FRONT_CHECK(sp, "HI1 front endpoint allocated");
        srvconn_ref(conns[1]); sp->aux = conns[1];
        if (!i) sp->flag |= CSRVCLIENT;
        hidx_t fd = handle_alloc(host, KOBJ_SPOOR, RIGHT_READ, sp);
        if (fd < 0) spoor_clunk(sp);
        PTI_FRONT_CHECK(fd >= 0, "HI1 front endpoint handle");
        if (!i) client_fd = fd; else server_fd = fd;
    }
    PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_BIND,
        master_fd, server_fd, 0) == -T_E_INVAL, "HI1 server endpoint is not observer proof");
    roots[1]->qid.path = 55;
    PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_BIND,
        master_fd, root_fd, 0) == -T_E_INVAL, "HI1 non-root dev9p observer refused");
    roots[1]->qid.path = 0;
    roots[0]->qid.path = 941;
    PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_BIND,
        master_fd, client_fd, 0) == -T_E_ACCES, "HI1 registered slave cannot bind");
    roots[0]->qid.path = 940;
    for (unsigned mode = 0; mode < 2; ++mode) {
        binding = sys_pty_register_for_proc(host, PTY_INTERACTION_BIND,
            master_fd, mode ? root_fd : client_fd, 0);
        PTI_FRONT_CHECK(binding > 0, "HI1 real master and observer front admitted");
        struct pts_interaction_call c = { .binding_id = (u64)binding };
        PTI_FRONT_CHECK(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &c) == 0,
                        "HI1 observer derived from service poster");
        PTI_FRONT_CHECK(c.state.binder_stripes == proc_stripes(host) &&
            c.state.pts_id == (u64)terminal, "HI1 front identities match held transport");
        PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_UNBIND,
            binding, 0, 0) == 0, "HI1 native front unbind");
        binding = -1;
    }
    binding = sys_pty_register_for_proc(host, PTY_INTERACTION_BIND,
        master_fd, root_fd, 0);
    PTI_FRONT_CHECK(binding > 0, "HI1 front allocation rollback binding");
    // The boot test thread is kproc and has no EL0 mapping at this address
    // (the existing test_uaccess fixture). Exercise real copyin/copyout fixup,
    // not merely the public range check or a kernel-pointer substitute.
    PTI_FRONT_CHECK(current_thread()->proc == observer, "HI1 native front caller is observer");
    PTI_FRONT_CHECK(sys_pty_register_test_native(PTY_INTERACTION_STATE,
        binding, 0x10000000ull, 80) == -T_E_FAULT, "HI1 STATE copyout fault reported");
    PTI_FRONT_CHECK(sys_pty_register_test_native(PTY_INTERACTION_ACK,
        binding, 0x10000000ull, 24) == -T_E_FAULT, "HI1 ACK copyin fault reported");
    PTI_FRONT_CHECK(sys_pty_register_test_native(PTY_INTERACTION_CHECK,
        binding, 0x10000000ull, 24) == -T_E_FAULT, "HI1 CHECK copyin fault reported");
    PTI_FRONT_CHECK(sys_pty_register_test_native(PTY_INTERACTION_ACK,
        binding, 0x10000000ull, 23) == -T_E_INVAL, "HI1 record size refused before copyin");
    PTI_FRONT_CHECK(sys_pty_register_test_native(PTY_INTERACTION_STATE,
        binding, ~(u64)0, 80) == -T_E_FAULT, "HI1 wrapping user range refused");
    struct pts_interaction_call after_fault = { .binding_id = (u64)binding };
    PTI_FRONT_CHECK(proc_pts_interaction(observer, PTY_INTERACTION_STATE, &after_fault) == 0 &&
        after_fault.state.acknowledged_epoch == 0 && after_fault.state.subject_stripes == 0,
        "HI1 failed copyin leaves acknowledgement unchanged");
    hidx_t last = -1;
    for (;;) {
        spoor_ref(roots[0]);
        hidx_t fd = handle_alloc(host, KOBJ_SPOOR, RIGHT_READ, roots[0]);
        if (fd < 0) { spoor_clunk(roots[0]); break; }
        last = fd;
    }
    PTI_FRONT_CHECK(last >= 0, "HI1 handle table filled");
    PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_WATCH,
        binding, 0, 0) == -T_E_NOMEM, "HI1 WATCH handle allocation failure reported");
    PTI_FRONT_CHECK(handle_close(host, last) == 0, "HI1 release one handle slot");
    s64 watch_fd = sys_pty_register_for_proc(host, PTY_INTERACTION_WATCH, binding, 0, 0);
    PTI_FRONT_CHECK(watch_fd >= 0, "HI1 failed WATCH releases reservation for retry");
    PTI_FRONT_CHECK(sys_pty_register_for_proc(host, PTY_INTERACTION_WATCH,
        binding, 0, 0) == -T_E_BUSY, "HI1 successful WATCH owns exactly one reservation");
cleanup:
    // Keep failures local: release fixtures before recording an assertion. The
    // negative legs must not leave live Procs for later lineage tests to visit.
    if (binding > 0 && host) {
        struct pts_interaction_call c = { .binding_id = (u64)binding };
        (void)proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c);
    }
    if (terminal > 0) (void)pts_free(host, (u64)terminal);
    if (host) for (int fd = 0; fd < PROC_HANDLE_MAX; ++fd) (void)handle_close(host, fd);
    for (unsigned i = 0; i < 2; ++i) {
        if (roots[i]) spoor_clunk(roots[i]);
        if (initialized[i]) {
            (void)p9_client_close(&pti_front_clients[i]);
            p9_client_destroy(&pti_front_clients[i]);
        } else if (transports[i].cn) {
            struct p9_transport_ops ops = p9_srvconn_transport_ops(&transports[i]);
            (void)ops.close(ops.ctx);
        }
        p9_srvconn_transport_destroy(&transports[i]);
        pts_drop_conn(conns[i]);
    }
    if (linked) pts_drop_linked(host); else pts_drop_proc(host);
    TEST_ASSERT(!failure, failure);
#undef PTI_FRONT_CHECK
}

// The retirer holds only the binding locator, not a watcher Spoor. Meanwhile
// the test thread unregisters the last poll hook, closes the last watch, and
// tries a new binding. This exercises the post-unlock wake pin against both
// the poll stack lifetime and reuse of the static binding pool.
static struct {
    u64 binding;
    volatile u32 issued, finished;
    volatile bool stop, exited;
    s64 result;
} pti_retire_job;

static void pti_retire_worker(void) {
    u32 seen = 0;
    while (!__atomic_load_n(&pti_retire_job.stop, __ATOMIC_ACQUIRE)) {
        u32 issued = __atomic_load_n(&pti_retire_job.issued, __ATOMIC_ACQUIRE);
        if (issued == seen) { sched(); continue; }
        struct pts_interaction_call c = { .binding_id = pti_retire_job.binding };
        pti_retire_job.result = proc_pts_interaction(kproc(), PTY_INTERACTION_UNBIND, &c);
        seen = issued;
        __atomic_store_n(&pti_retire_job.finished, seen, __ATOMIC_RELEASE);
    }
    test_kthread_park_terminal(&pti_retire_job.exited);
}

static void pts_test_interaction_retirement_race(void) {
    const char *failure = NULL;
    struct Proc *host = proc_alloc();
    struct SrvConn *cn = NULL;
    struct Spoor *watch = NULL;
    struct Thread *worker = NULL;
    bool linked = false, registered = false;
    s64 terminal = -1, binding = -1;
    struct poll_waiter waiter;
    struct Rendez rendez;
#define PTI_RACE_CHECK(condition, message) \
    do { if (!(condition)) { failure = message; goto cleanup; } } while (0)
    PTI_RACE_CHECK(host, "HI1 race host allocated");
    proc_test_link(host); linked = true;
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    cn = pts_make_conn(host);
    PTI_RACE_CHECK(cn, "HI1 race transport allocated");
    terminal = pts_mint(host, cn, 950);
    PTI_RACE_CHECK(terminal > 0, "HI1 race terminal minted");
    pti_retire_job.issued = pti_retire_job.finished = 0;
    pti_retire_job.stop = pti_retire_job.exited = false;
    worker = thread_create(kproc(), pti_retire_worker);
    PTI_RACE_CHECK(worker, "HI1 race worker created");
    ready(worker);
    for (u32 iteration = 1; iteration <= 256; ++iteration) {
        struct pts_interaction_call c = { .pts_id = (u64)terminal,
            .observer_stripes = proc_stripes(kproc()) };
        binding = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
        PTI_RACE_CHECK(binding > 0, "HI1 race binding created");
        s64 error;
        watch = pts_interaction_watch(kproc(), (u64)binding, &error);
        PTI_RACE_CHECK(watch, "HI1 race watch created");
        struct t_pty_interaction_state state;
        PTI_RACE_CHECK(pti_test_read(kproc(), watch, &state, sizeof(state)) == sizeof(state),
                        "HI1 race initial revision consumed");
        rendez_init(&rendez);
        poll_waiter_init(&waiter, &rendez);
        short events = pti_test_poll(kproc(), watch, &waiter);
        registered = waiter.list != NULL;
        PTI_RACE_CHECK(events == 0 && registered, "HI1 race poll armed before retire");
        pti_retire_job.binding = (u64)binding;
        __atomic_store_n(&pti_retire_job.issued, iteration, __ATOMIC_RELEASE);
        poll_waiter_list_unregister(&waiter); registered = false;
        spoor_clunk(watch); watch = NULL;
        // Rebinding races the writer's post-unlock wake. Busy is allowed only
        // until retirement has actually committed; there is no sleep in a lock.
        s64 replacement = -T_E_BUSY;
        u64 deadline = timer_now_ns() + TEST_YIELD_BUDGET_NS;
        while (replacement == -T_E_BUSY && timer_now_ns() < deadline) {
            replacement = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
            if (replacement == -T_E_BUSY) sched();
        }
        if (replacement > 0) binding = replacement;
        PTI_RACE_CHECK(replacement > 0, "HI1 race replacement eventually admitted");
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&pti_retire_job.finished, __ATOMIC_ACQUIRE) == iteration);
        PTI_RACE_CHECK(__atomic_load_n(&pti_retire_job.finished, __ATOMIC_ACQUIRE) == iteration &&
            pti_retire_job.result == 0, "HI1 race retirement completed");
        c.binding_id = (u64)replacement;
        PTI_RACE_CHECK(proc_pts_interaction(kproc(), PTY_INTERACTION_STATE, &c) == 0 &&
            c.state.binding_id == (u64)replacement && c.state.revision == 1,
            "HI1 race old wake cannot retire or mutate replacement");
        PTI_RACE_CHECK(proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c) == 0,
                        "HI1 race replacement released");
        binding = -1;
    }
cleanup:
    // Stop/join before reclaiming any fixture the worker can name. Even an
    // assertion failure therefore cannot escape into later tests' lineage.
    if (worker) {
        __atomic_store_n(&pti_retire_job.stop, true, __ATOMIC_RELEASE);
        test_kthread_join_free(worker, &pti_retire_job.exited);
    }
    if (registered) poll_waiter_list_unregister(&waiter);
    if (watch) spoor_clunk(watch);
    if (binding > 0) {
        struct pts_interaction_call c = { .binding_id = (u64)binding };
        (void)proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c);
    }
    if (terminal > 0) (void)pts_free(host, (u64)terminal);
    pts_drop_conn(cn);
    if (linked) pts_drop_linked(host); else pts_drop_proc(host);
    TEST_ASSERT(!failure, failure);
#undef PTI_RACE_CHECK
}

static void pts_test_interaction_boundaries(void) {
    const char *failure = NULL;
    struct Proc *host = proc_alloc();
    struct SrvConn *cn = NULL;
    struct Spoor *watch = NULL;
    bool linked = false;
    s64 terminal = -1, binding = -1;
    u64 saved_next_id = 0;
#define PTI_EDGE_CHECK(condition, message) \
    do { if (!(condition)) { failure = message; goto cleanup; } } while (0)
    PTI_EDGE_CHECK(host, "HI1 edge host allocated");
    proc_test_link(host); linked = true;
    proc_seal(host, PROC_FLAG_NOTRACE | PROC_FLAG_NODUMP);
    cn = pts_make_conn(host);
    PTI_EDGE_CHECK(cn, "HI1 edge transport allocated");
    for (unsigned mode = 0; mode < 3; ++mode) {
        terminal = pts_mint(host, cn, 960);
        PTI_EDGE_CHECK(terminal > 0, "HI1 edge terminal minted");
        PTI_EDGE_CHECK(pts_bind_slave(host, cn, 961, (u64)terminal) == 0,
                       "HI1 edge slave registered");
        if (mode == 2) {
            saved_next_id = pts_interaction_test_exchange_next_id(PTY_INTERACTION_ID_MAX);
            PTI_EDGE_CHECK(saved_next_id, "HI1 ID boundary fixture requires empty pool");
        }
        struct pts_interaction_call c = { .pts_id = (u64)terminal,
            .observer_stripes = proc_stripes(kproc()) };
        binding = proc_pts_interaction(host, PTY_INTERACTION_BIND, &c);
        PTI_EDGE_CHECK(binding > 0, "HI1 edge binding created");
        c.binding_id = (u64)binding;
        s64 error;
        watch = pts_interaction_watch(kproc(), (u64)binding, &error);
        PTI_EDGE_CHECK(watch, "HI1 edge watcher created");
        if (mode == 0) {
            PTI_EDGE_CHECK(pts_interaction_test_counters(binding, ~(u64)0, 1),
                           "HI1 epoch boundary injected");
            PTI_EDGE_CHECK(pts_tty_acquire(host, cn, 961) == 0,
                           "HI1 epoch exhaustion preserves terminal acquisition");
            PTI_EDGE_CHECK(pts_tty_set_fg(host, cn, 961, host->pgid) == 0,
                           "HI1 epoch exhaustion preserves ordinary job control");
            PTI_EDGE_CHECK(proc_pts_interaction(host, PTY_INTERACTION_BIND, &c) == -T_E_NOSPC,
                           "HI1 exhausted terminal does not wrap its epoch");
        } else if (mode == 1) {
            PTI_EDGE_CHECK(pts_interaction_test_counters(binding, 7, ~(u64)0),
                           "HI1 revision boundary injected");
            c.request = (struct t_pty_interaction_check){ .version = 1, .size = 24,
                .expected_epoch = 7, .subject_stripes = 0 };
            PTI_EDGE_CHECK(proc_pts_interaction(kproc(), PTY_INTERACTION_ACK, &c) == -T_E_NOENT,
                           "HI1 revision exhaustion retires before acknowledgement");
        } else {
            PTI_EDGE_CHECK((u64)binding == PTY_INTERACTION_ID_MAX,
                           "HI1 last ID remains a positive success value");
            PTI_EDGE_CHECK(proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c) == 0,
                           "HI1 last ID can be revoked");
            PTI_EDGE_CHECK(proc_pts_interaction(host, PTY_INTERACTION_BIND, &c) == -T_E_NOSPC,
                           "HI1 ID exhaustion never enters errno space");
        }
        PTI_EDGE_CHECK(pti_test_poll(kproc(), watch, NULL) == POLLHUP,
                       "HI1 exhausted binding wakes retirement");
        PTI_EDGE_CHECK(proc_pts_interaction(kproc(), PTY_INTERACTION_STATE, &c) == -T_E_NOENT,
                       "HI1 exhausted binding has no admission state");
        spoor_clunk(watch); watch = NULL; binding = -1;
        PTI_EDGE_CHECK(pts_free(host, (u64)terminal) == 0, "HI1 edge terminal released");
        terminal = -1;
    }
cleanup:
    if (binding > 0) {
        struct pts_interaction_call c = { .binding_id = (u64)binding };
        (void)proc_pts_interaction(host, PTY_INTERACTION_UNBIND, &c);
    }
    if (watch) spoor_clunk(watch);
    if (terminal > 0) (void)pts_free(host, (u64)terminal);
    if (saved_next_id && !pts_interaction_test_exchange_next_id(saved_next_id))
        failure = "HI1 ID fixture could not restore empty-pool allocator";
    pts_drop_conn(cn);
    if (linked) pts_drop_linked(host); else pts_drop_proc(host);
    TEST_ASSERT(!failure, failure);
#undef PTI_EDGE_CHECK
}
