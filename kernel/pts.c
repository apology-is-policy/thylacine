// The pts registry (PTY-1c). See <thylacine/pts.h> for the model, the
// authority anchors, and the locking contract; PTY-DESIGN.md section 3 is
// the binding design.

#include <thylacine/pts.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>

struct pts_binding {
    bool            used;
    bool            master;    // the mint-side binding vs a slave-serve one
    struct SrvConn *conn;      // srvconn_ref held while bound (no ABA)
    u64             qid;       // qid.path on that connection
};

struct pts_entry {
    bool live;
    u32  gen;          // 0 = virgin slot; stamped >= 1 at mint; bumped at free
    u32  server_pid;   // the minting server (pids are never reused)
    struct pts_binding bindings[PTS_BINDINGS_MAX];
    // Controlling-terminal state -- kernel-owned (the F1 seam). Zeroed at
    // mint; PTY-1d's acquisition / tcsetpgrp are the only mutators. 0 = none.
    u32  ct_sid;
    u32  fg_pgid;
};

static struct pts_entry g_pts[PTS_MAX];
static spin_lock_t      g_pts_lock;

// Decode + validate a pts_id against the live registry. Lock held by caller.
static struct pts_entry *pts_lookup_locked(u64 pts_id) {
    u32 idx = (u32)(pts_id & ((1u << PTS_IDX_BITS) - 1u));
    u32 gen = (u32)(pts_id >> PTS_IDX_BITS);
    if (pts_id == 0 || idx >= PTS_MAX || gen == 0) return NULL;
    struct pts_entry *e = &g_pts[idx];
    if (!e->live || e->gen != gen) return NULL;
    return e;
}

static u64 pts_id_of_locked(const struct pts_entry *e) {
    u32 idx = (u32)(e - g_pts);
    return ((u64)e->gen << PTS_IDX_BITS) | idx;
}

// True iff (cn, qid) is bound on any LIVE entry; reports where. Lock held.
static struct pts_entry *pts_find_binding_locked(struct SrvConn *cn, u64 qid,
                                                 struct pts_binding **b_out) {
    for (u32 i = 0; i < PTS_MAX; i++) {
        struct pts_entry *e = &g_pts[i];
        if (!e->live) continue;
        for (u32 j = 0; j < PTS_BINDINGS_MAX; j++) {
            struct pts_binding *b = &e->bindings[j];
            if (b->used && b->conn == cn && b->qid == qid) {
                if (b_out) *b_out = b;
                return e;
            }
        }
    }
    return NULL;
}

// Clear an entry: stage its binding conns for the caller to srvconn_unref
// AFTER g_pts_lock drops (the last unref tears down + frees -- chan/slab
// locks; never under the registry leaf). Bumps the gen so every stale id
// fails from here on. Lock held.
static int pts_clear_locked(struct pts_entry *e,
                            struct SrvConn *drop[PTS_BINDINGS_MAX]) {
    int ndrop = 0;
    for (u32 j = 0; j < PTS_BINDINGS_MAX; j++) {
        if (e->bindings[j].used) drop[ndrop++] = e->bindings[j].conn;
        e->bindings[j] = (struct pts_binding){0};
    }
    e->live       = false;
    e->server_pid = 0;
    e->ct_sid     = 0;
    e->fg_pgid    = 0;
    e->gen++;
    if (e->gen == 0) e->gen = 1;   // u32 wrap backstop; 0 stays "virgin"
    return ndrop;
}

// Reclaim ONE entry whose every binding conn is torn (a dead server's conns
// are TORN by its handle-close teardown), making room for the caller's mint.
// A live slave conn on an otherwise-dead entry blocks reclaim -- conservative;
// the pts may still be serving through that side. srvconn_is_live reads
// cn->state (LIVE -> TORN is monotonic), a leaf nested under g_pts_lock.
// PTY-1d seam: entries reclaimed here carried inert ct_sid/fg_pgid at 1c;
// once the tty:hup teardown path lands (1d), server-death hup delivery runs
// on the conn-teardown path itself, so GC'd entries are already hup'd.
static struct pts_entry *pts_gc_one_locked(struct SrvConn *drop[PTS_BINDINGS_MAX],
                                           int *ndrop_out) {
    for (u32 i = 0; i < PTS_MAX; i++) {
        struct pts_entry *e = &g_pts[i];
        if (!e->live) continue;
        bool all_torn = true;
        for (u32 j = 0; j < PTS_BINDINGS_MAX && all_torn; j++) {
            if (e->bindings[j].used && srvconn_is_live(e->bindings[j].conn))
                all_torn = false;
        }
        if (all_torn) {
            *ndrop_out = pts_clear_locked(e, drop);
            return e;
        }
    }
    return NULL;
}

s64 pts_mint(struct Proc *server, struct SrvConn *cn, u64 master_qid) {
    if (!server || !cn || master_qid == 0) return -T_E_INVAL;

    struct SrvConn *drop[PTS_BINDINGS_MAX];
    int ndrop = 0;
    s64 ret;

    spin_lock(&g_pts_lock);
    if (pts_find_binding_locked(cn, master_qid, NULL)) {
        ret = -T_E_EXIST;
        goto out;
    }
    struct pts_entry *e = NULL;
    for (u32 i = 0; i < PTS_MAX; i++) {
        if (!g_pts[i].live) { e = &g_pts[i]; break; }
    }
    if (!e) e = pts_gc_one_locked(drop, &ndrop);
    if (!e) {
        ret = -T_E_AGAIN;
        goto out;
    }
    if (e->gen == 0) e->gen = 1;   // virgin slot's first generation
    e->live       = true;
    e->server_pid = (u32)server->pid;
    e->ct_sid     = 0;
    e->fg_pgid    = 0;
    srvconn_ref(cn);               // atomic; safe under the spinlock
    e->bindings[0] = (struct pts_binding){
        .used = true, .master = true, .conn = cn, .qid = master_qid,
    };
    ret = (s64)pts_id_of_locked(e);
out:
    spin_unlock(&g_pts_lock);
    for (int k = 0; k < ndrop; k++) srvconn_unref(drop[k]);
    return ret;
}

int pts_bind_slave(struct Proc *server, struct SrvConn *cn, u64 slave_qid,
                   u64 pts_id) {
    if (!server || !cn || slave_qid == 0) return -T_E_INVAL;

    int ret;
    spin_lock(&g_pts_lock);
    struct pts_entry *e = pts_lookup_locked(pts_id);
    if (!e) {
        ret = -T_E_INVAL;
        goto out;
    }
    if (e->server_pid != (u32)server->pid) {
        ret = -T_E_ACCES;
        goto out;
    }
    struct pts_entry *holder = pts_find_binding_locked(cn, slave_qid, NULL);
    if (holder == e) {
        ret = 0;                   // identical re-bind: idempotent
        goto out;
    }
    if (holder) {
        ret = -T_E_EXIST;          // bound to a DIFFERENT live pts
        goto out;
    }
    struct pts_binding *b = NULL;
    for (u32 j = 0; j < PTS_BINDINGS_MAX; j++) {
        if (!e->bindings[j].used) { b = &e->bindings[j]; break; }
    }
    if (!b) {
        ret = -T_E_NOMEM;
        goto out;
    }
    srvconn_ref(cn);
    *b = (struct pts_binding){
        .used = true, .master = false, .conn = cn, .qid = slave_qid,
    };
    ret = 0;
out:
    spin_unlock(&g_pts_lock);
    return ret;
}

int pts_free(struct Proc *server, u64 pts_id) {
    if (!server) return -T_E_INVAL;

    struct SrvConn *drop[PTS_BINDINGS_MAX];
    int ndrop = 0;
    int ret;

    spin_lock(&g_pts_lock);
    struct pts_entry *e = pts_lookup_locked(pts_id);
    if (!e) {
        ret = -T_E_INVAL;
    } else if (e->server_pid != (u32)server->pid) {
        ret = -T_E_ACCES;
    } else {
        ndrop = pts_clear_locked(e, drop);
        ret = 0;
    }
    spin_unlock(&g_pts_lock);
    for (int k = 0; k < ndrop; k++) srvconn_unref(drop[k]);
    return ret;
}

s64 pts_resolve_conn_qid(struct SrvConn *cn, u64 qid, bool *is_master_out) {
    if (!cn || qid == 0) return -T_E_INVAL;

    s64 ret = -T_E_NOENT;
    spin_lock(&g_pts_lock);
    struct pts_binding *b = NULL;
    struct pts_entry *e = pts_find_binding_locked(cn, qid, &b);
    if (e) {
        if (is_master_out) *is_master_out = b->master;
        ret = (s64)pts_id_of_locked(e);
    }
    spin_unlock(&g_pts_lock);
    return ret;
}

s64 pts_resolve_spoor(struct Spoor *sp, bool *is_master_out) {
    if (!sp) return -T_E_INVAL;
    struct p9_client *cl = NULL;
    if (dev9p_client_fid(sp, &cl, NULL) != 0 || !cl) return -T_E_INVAL;
    // The transport downcast: NULL for a loopback / spoor-backed client --
    // those sessions carry no SrvConn, so no pts can be registered on them
    // and the resolve fails closed. The returned pointer is only ever
    // pointer-compared against ref-held bindings (pts.h contract).
    struct SrvConn *cn = p9_srvconn_transport_conn(cl);
    if (!cn) return -T_E_NOENT;
    return pts_resolve_conn_qid(cn, sp->qid.path, is_master_out);
}
