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
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/pts.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>   // PTY-1f: fabricated member Threads
#include <thylacine/types.h>

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
