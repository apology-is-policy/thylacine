// The pts registry (PTY-1c). See <thylacine/pts.h> for the model, the
// authority anchors, and the locking contract; PTY-DESIGN.md section 3 is
// the binding design.

#include <thylacine/pts.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/dev9p.h>
#include <thylacine/errno.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/syscall.h>   // the TTY_SIG_* class ABI

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
// locks; never under the registry leaf), and stage the controlling-terminal
// snapshot (ct_sid, fg_pgid) for the caller's F8 teardown fan -- ALSO after
// release (the F11 no-nesting discipline: no note post / proc-table walk
// under g_pts_lock). Bumps the gen so every stale id fails from here on.
// Lock held.
static int pts_clear_locked(struct pts_entry *e,
                            struct SrvConn *drop[PTS_BINDINGS_MAX],
                            u32 *ct_sid_out, u32 *fg_out) {
    int ndrop = 0;
    for (u32 j = 0; j < PTS_BINDINGS_MAX; j++) {
        if (e->bindings[j].used) drop[ndrop++] = e->bindings[j].conn;
        e->bindings[j] = (struct pts_binding){0};
    }
    if (ct_sid_out) *ct_sid_out = e->ct_sid;
    if (fg_out)     *fg_out     = e->fg_pgid;
    e->live       = false;
    e->server_pid = 0;
    e->ct_sid     = 0;
    e->fg_pgid    = 0;
    e->gen++;
    if (e->gen == 0) e->gen = 1;   // u32 wrap backstop; 0 stays "virgin"
    return ndrop;
}

// F8 (PTY-1f): the pts-teardown carrier-loss fan, run with g_pts_lock
// RELEASED on the (ct_sid, fg) snapshot pts_clear_locked staged. The pts is
// gone (explicit FREE, or GC of a dead server's entry) -- POSIX modem-hangup:
// tty:hup to the foreground group AND (F13) the controlling process (the
// session leader) when it sits outside the fg group, THEN tty:cont + the
// job-resume to the fg group, per-member hup-before-cont (POSIX 2.4.3's
// order -- the resume lets an uncaught hup's terminate actually run, and a
// hup-catching survivor actually handle it; "no group stranded with a dead
// SIGCONT source"). Groups stopped via this pts that are NOT the fg at
// teardown (the shell re-seated itself after a ^Z -- the standard sequence)
// are covered by the composition, not tracked provenance: the shell/leader
// receives this hup; if it dies, proc_become_zombie_locked's orphan rule
// delivers hup+cont to its newly-orphaned stopped groups; if it catches the
// hup and survives, its slave fd still resolves (the registry compares
// POINTERS -- a TORN conn still matches), so SYS_TTY_CONT keeps working...
// on the still-LIVE entry. Once the entry is FREED that path dies with it --
// the hup-surviving-shell-with-stopped-bg-jobs corner is a recorded v1.x
// seam (a kill-authority SIGCONT). `caller` feeds proc_getpgid's leader
// lookup (any Proc may query -- PTY-1a).
static void pts_teardown_fan(struct Proc *caller, u32 ct_sid, u32 fg) {
    if (ct_sid == 0 || fg == 0) return;   // nobody controlled this terminal
    (void)notes_post_pgrp(fg, NOTE_NAME_TTY_HUP, 0);
    s64 lp = proc_getpgid(caller, (int)ct_sid);
    if (lp > 0 && (u32)lp != fg)
        (void)notes_post_pid((int)ct_sid, NOTE_NAME_TTY_HUP, 0);
    (void)proc_job_cont_pgrp(fg);
}

// Reclaim ONE entry whose every binding conn is torn (a dead server's conns
// are TORN by its handle-close teardown), making room for the caller's mint.
// A live slave conn on an otherwise-dead entry blocks reclaim -- conservative;
// the pts may still be serving through that side. srvconn_is_live reads
// cn->state (LIVE -> TORN is monotonic), a leaf nested under g_pts_lock.
// PTY-1f (F8): the reclaimed entry's (ct_sid, fg) is staged for the caller's
// post-release pts_teardown_fan -- a dead server's controlled session gets
// its carrier-loss hup + the stopped-fg resume at GC time (lazy: at the next
// mint-full; the interim is covered by the orphan rule + the torn-conn
// SYS_TTY_CONT resolve, see pts_teardown_fan's composition note).
static struct pts_entry *pts_gc_one_locked(struct SrvConn *drop[PTS_BINDINGS_MAX],
                                           int *ndrop_out,
                                           u32 *ct_sid_out, u32 *fg_out) {
    for (u32 i = 0; i < PTS_MAX; i++) {
        struct pts_entry *e = &g_pts[i];
        if (!e->live) continue;
        bool all_torn = true;
        for (u32 j = 0; j < PTS_BINDINGS_MAX && all_torn; j++) {
            if (e->bindings[j].used && srvconn_is_live(e->bindings[j].conn))
                all_torn = false;
        }
        if (all_torn) {
            *ndrop_out = pts_clear_locked(e, drop, ct_sid_out, fg_out);
            return e;
        }
    }
    return NULL;
}

s64 pts_mint(struct Proc *server, struct SrvConn *cn, u64 master_qid) {
    if (!server || !cn || master_qid == 0) return -T_E_INVAL;

    struct SrvConn *drop[PTS_BINDINGS_MAX];
    int ndrop = 0;
    u32 gc_ct_sid = 0, gc_fg = 0;   // the GC'd entry's F8 fan snapshot
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
    if (!e) e = pts_gc_one_locked(drop, &ndrop, &gc_ct_sid, &gc_fg);
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
    // F8: the GC victim's carrier-loss fan -- after the lock + the unrefs
    // (the fan takes g_proc_table_lock + note queue locks; never under the
    // registry leaf). The minter is the proc_getpgid query caller.
    pts_teardown_fan(server, gc_ct_sid, gc_fg);
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
    u32 ct_sid = 0, fg = 0;
    int ret;

    spin_lock(&g_pts_lock);
    struct pts_entry *e = pts_lookup_locked(pts_id);
    if (!e) {
        ret = -T_E_INVAL;
    } else if (e->server_pid != (u32)server->pid) {
        ret = -T_E_ACCES;
    } else {
        ndrop = pts_clear_locked(e, drop, &ct_sid, &fg);
        ret = 0;
    }
    spin_unlock(&g_pts_lock);
    for (int k = 0; k < ndrop; k++) srvconn_unref(drop[k]);
    // F8: the explicit-FREE (last-master-close) carrier-loss fan, after the
    // lock + the unrefs. (ct_sid, fg) stay 0 on the error arms -> no-op.
    pts_teardown_fan(server, ct_sid, fg);
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

int pts_spoor_conn_qid(struct Spoor *sp, struct SrvConn **cn_out,
                       u64 *qid_out) {
    if (!sp || !cn_out || !qid_out) return -T_E_INVAL;
    struct p9_client *cl = NULL;
    if (dev9p_client_fid(sp, &cl, NULL) != 0 || !cl) return -T_E_INVAL;
    // The transport downcast: NULL for a loopback / spoor-backed client --
    // those sessions carry no SrvConn, so no pts can be registered on them
    // and the resolve fails closed. The returned pointer is only ever
    // pointer-compared against ref-held bindings (pts.h contract).
    struct SrvConn *cn = p9_srvconn_transport_conn(cl);
    if (!cn) return -T_E_NOENT;
    *cn_out  = cn;
    *qid_out = sp->qid.path;
    return 0;
}

s64 pts_resolve_spoor(struct Spoor *sp, bool *is_master_out) {
    struct SrvConn *cn = NULL;
    u64 qid = 0;
    int rc = pts_spoor_conn_qid(sp, &cn, &qid);
    if (rc != 0) return (s64)rc;
    return pts_resolve_conn_qid(cn, qid, is_master_out);
}

// =============================================================================
// PTY-1d: the tty seam + controlling-terminal cores (pts.h contracts).
// =============================================================================

s64 pts_tty_signal(struct Proc *server, u64 pts_id, u32 sig_class) {
    if (!server) return -T_E_INVAL;
    if (sig_class < TTY_SIG_INT || sig_class > TTY_SIG_HUP) return -T_E_INVAL;

    u32 ct_sid, fg;
    spin_lock(&g_pts_lock);
    struct pts_entry *e = pts_lookup_locked(pts_id);
    if (!e) {
        spin_unlock(&g_pts_lock);
        return -T_E_INVAL;
    }
    if (e->server_pid != (u32)server->pid) {
        spin_unlock(&g_pts_lock);
        return -T_E_ACCES;
    }
    ct_sid = e->ct_sid;
    fg     = e->fg_pgid;
    spin_unlock(&g_pts_lock);

    // PTY-1f: the suspend class is LIVE -- the job-control stop fan-out
    // (proc_job_stop_pgrp: per-member catchability gate [a caught susp is a
    // note, not a stop -- R2-F3] + the orphaned-group discard + the default
    // STOP with the PTY-1e stop report). Runs with g_pts_lock RELEASED on
    // the snapshot, exactly like the note fans below (the F11 no-nesting
    // discipline). No controlling session / no fg seated -> 0, like INT.
    if (sig_class == TTY_SIG_TSTP) {
        if (ct_sid == 0 || fg == 0) return 0;
        return (s64)proc_job_stop_pgrp(fg);
    }

    const char *name;
    switch (sig_class) {
    case TTY_SIG_INT:   name = NOTE_NAME_INTERRUPT; break;
    case TTY_SIG_QUIT:  name = NOTE_NAME_TTY_QUIT;  break;
    case TTY_SIG_WINCH: name = NOTE_NAME_TTY_WINCH; break;
    default:            name = NOTE_NAME_TTY_HUP;   break;
    }
    if (ct_sid == 0 || fg == 0) return 0;   // nobody controls this terminal

    s64 n = (s64)notes_post_pgrp(fg, name, 0);
    if (sig_class == TTY_SIG_HUP) {
        // F13: carrier loss also reaches the controlling process (the
        // session leader, pid == ct_sid) when it is not in the foreground
        // group. proc_getpgid answers for a ZOMBIE leader too; the ALIVE
        // gate lives in notes_post_pid.
        s64 lp = proc_getpgid(server, (int)ct_sid);
        if (lp > 0 && (u32)lp != fg)
            n += (s64)notes_post_pid((int)ct_sid, name, 0);
    }
    return n;
}

s64 pts_tty_acquire(struct Proc *p, struct SrvConn *cn, u64 qid) {
    if (!p || !cn || qid == 0) return -T_E_INVAL;
    if (p->sid != (u32)p->pid) return -T_E_ACCES;   // not a session leader
    // The leader's own sid is self-stable (only setsid mutates it, self-
    // only); pgid is racy-benign in principle but a session leader's pgid
    // is pinned == pid (setpgid rejects session-leader targets).
    u32 sid  = p->sid;
    u32 pgid = p->pgid;

    s64 ret;
    spin_lock(&g_pts_lock);
    struct pts_binding *b = NULL;
    struct pts_entry *e = pts_find_binding_locked(cn, qid, &b);
    if (!e) {
        ret = -T_E_NOENT;
    } else if (b->master) {
        ret = -T_E_INVAL;              // acquisition is a slave-side act
    } else if (e->ct_sid == sid) {
        ret = 0;                       // already ours: the second open inherits
    } else if (e->ct_sid != 0) {
        ret = -T_E_ACCES;              // another session's terminal: no steal
    } else {
        // One session, at most one controlling terminal (POSIX): reject if
        // this session already controls a different pts.
        bool has = false;
        for (u32 i = 0; i < PTS_MAX && !has; i++) {
            if (g_pts[i].live && g_pts[i].ct_sid == sid) has = true;
        }
        if (has) {
            ret = -T_E_ACCES;
        } else {
            e->ct_sid  = sid;
            e->fg_pgid = pgid;
            ret = 0;
        }
    }
    spin_unlock(&g_pts_lock);
    return ret;
}

s64 pts_tty_set_fg(struct Proc *p, struct SrvConn *cn, u64 qid, u32 pgid) {
    if (!p || !cn || qid == 0)  return -T_E_INVAL;
    if (pgid == 0 || (s32)pgid < 0) return -T_E_INVAL;
    // The membership gate runs UNLOCKED before the seat (no g_pts_lock ->
    // g_proc_table_lock nesting); the group-empties-in-the-window race is
    // the benign POSIX one (pts.h contract).
    if (!proc_pgrp_in_session(pgid, p->sid)) return -T_E_ACCES;

    s64 ret;
    spin_lock(&g_pts_lock);
    struct pts_binding *b = NULL;
    struct pts_entry *e = pts_find_binding_locked(cn, qid, &b);
    if (!e) {
        ret = -T_E_NOENT;
    } else if (e->ct_sid == 0 || e->ct_sid != p->sid) {
        ret = -T_E_ACCES;              // not the caller's controlling terminal
    } else {
        e->fg_pgid = pgid;
        ret = 0;
    }
    spin_unlock(&g_pts_lock);
    return ret;
}

// PTY-1f (SYS_TTY_CONT = 98, user-signed-off 2026-07-17): the shell's
// `fg`/`bg` resume -- the ONE named path by which a session member resumes a
// job-stopped group in its session (F4 keeps tty:cont kernel-synthetic-only
// on the POST axis; F8 covers only the teardown cont; ordinary kill covers
// only one's OWN group). Gated EXACTLY like SET_FG: the membership check
// runs UNLOCKED before the seat lookup (no g_pts_lock -> g_proc_table_lock
// nesting; the group-empties-in-the-window race is the benign POSIX one),
// then the binding + controlling-session gates under the registry leaf,
// then -- with g_pts_lock RELEASED -- the proc_job_cont_pgrp fan (tty:cont
// note + the per-owner job resume; a debugger-stopped member re-parks:
// StopCompatI39). The target group need NOT be the fg (bg resumes a
// background job) and need not be stopped (the cont note still posts --
// POSIX SIGCONT semantics). Works on a TORN conn (the resolve is pure
// pointer identity), so a shell whose ptyfs died can still resume its jobs
// while the registry entry lives.
s64 pts_tty_cont(struct Proc *p, struct SrvConn *cn, u64 qid, u32 pgid) {
    if (!p || !cn || qid == 0)      return -T_E_INVAL;
    if (pgid == 0 || (s32)pgid < 0) return -T_E_INVAL;
    if (!proc_pgrp_in_session(pgid, p->sid)) return -T_E_ACCES;

    s64 ret;
    spin_lock(&g_pts_lock);
    struct pts_binding *b = NULL;
    struct pts_entry *e = pts_find_binding_locked(cn, qid, &b);
    if (!e) {
        ret = -T_E_NOENT;
    } else if (e->ct_sid == 0 || e->ct_sid != p->sid) {
        ret = -T_E_ACCES;              // not the caller's controlling terminal
    } else {
        ret = 0;
    }
    spin_unlock(&g_pts_lock);
    if (ret == 0)
        ret = (s64)proc_job_cont_pgrp(pgid);
    return ret;
}

s64 pts_tty_get_fg(struct Proc *p, struct SrvConn *cn, u64 qid) {
    if (!p || !cn || qid == 0) return -T_E_INVAL;

    s64 ret;
    spin_lock(&g_pts_lock);
    struct pts_binding *b = NULL;
    struct pts_entry *e = pts_find_binding_locked(cn, qid, &b);
    if (!e) {
        ret = -T_E_NOENT;
    } else if (!b->master && (e->ct_sid == 0 || e->ct_sid != p->sid)) {
        ret = -T_E_ACCES;              // a slave read needs session membership
    } else {
        ret = (s64)e->fg_pgid;         // 0 = none seated
    }
    spin_unlock(&g_pts_lock);
    return ret;
}
