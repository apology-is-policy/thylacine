// devsrv — the /srv service registry Dev + per-connection layer.
//
// Per ARCHITECTURE.md §9.4 + CORVUS-DESIGN.md §6. `/srv` is a kernel Dev
// (Plan 9's `#s`) by which a userspace 9P server registers a name with
// SYS_POST_SERVICE; the kernel mints + mediates per-connection client
// access. `devsrv` is deliberately distinct from `dev9p` so a Spoor
// walked out of `/srv` is structurally a KObj_Srv kernel object —
// non-transferable, which keeps the kernel-stamped peer identity behind
// it unforgeable.
//
// P5-corvus-srv-impl-a2 landed the service registry + the devsrv Dev's
// attach + SYS_POST_SERVICE. P5-corvus-srv-impl-a3b adds the
// per-connection layer:
//   - the accept backlog on struct SrvService;
//   - devsrv_open_connect — the open=connect path (SYS_OPEN on /srv/<name>):
//     mint a SrvConn, enqueue it, and return the connection endpoint Spoor
//     (a dev9p root for 9P-mode, a CSRVCLIENT conn Spoor for byte-mode);
//   - srv_accept_blocking — the poster's blocking accept (SYS_SRV_ACCEPT);
//   - the devsrv walk op + the connection-Spoor read/write/close ops;
//   - srv_proc_exit_notify draining a dead poster's backlog.
//
// stalk-3a (STALK-DESIGN.md §5.1) makes the registry NAMESPACE-RESIDENT:
// the single static registry becomes a heap-allocated, refcounted
// `SrvRegistry` reached THROUGH the mounted devsrv root Spoor (the root's
// `aux`), not a global. Boot mounts one immortal registry on the kproc
// `/srv` synthetic dir; a future login (A-5b-body) mounts a fresh
// per-session registry so a second user's coordinator is structurally
// unnameable (I-1). Posting is create=post (SYS_WALK_CREATE on a /srv dir
// -> devsrv_post_listener; the DMSRVBYTE perm bit selects byte- vs 9P-mode)
// and connecting is open=connect (SYS_OPEN on /srv/<name> -> devsrv_open_
// connect); the name-only SYS_POST_SERVICE / SYS_SRV_CONNECT syscalls were
// retired in stalk-3c. Every devsrv op resolves the registry from the
// Spoor's aux. Registry-ref discipline mirrors dev9p's attached_owner:
// every devsrv Spoor instance carrying `aux = reg` holds exactly ONE
// registry ref, dropped at devsrv_close (which fires only on the Spoor's
// last clunk). The boot registry never frees (kproc's mount holds it
// forever).
//
// Spec: specs/corvus.tla — MarkMayPost / PostService / ServiceTombstone /
// SrvBind / SrvAccept / ProcExit; specs/handles.tla — KObj_Srv.

#include <thylacine/9p_attach.h>
#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// The registry. Heap-allocated by srv_registry_create (stalk-3a); was a
// single static struct pre-stalk-3a. `magic` (offset 0) discriminates a
// devsrv root Spoor's aux (SRV_REGISTRY_MAGIC) from a service-ref
// (DEVSRV_SVC_MAGIC) / connection (SRV_CONN_MAGIC). `ref` is the
// instance-scoped refcount: the mounted root, each cross-clone of it, and
// each /srv/<name> service-ref each hold one (see the file header). Each
// entry's `state` is FREE (== 0) at create (KP_ZERO); srv_registry_create
// stamps each entry's permanent magic + poll_list + reg back-pointer.
struct SrvRegistry {
    u64               magic;          // SRV_REGISTRY_MAGIC; 0 once freed
    int               ref;            // instance refcount (atomic)
    spin_lock_t       lock;
    struct SrvService entries[SRV_MAX_SERVICES];
};

_Static_assert(__builtin_offsetof(struct SrvRegistry, magic) == 0,
               "magic at offset 0 — a devsrv root Spoor's aux is its "
               "SrvRegistry; devsrv_close / devsrv_poll / devsrv_conn_of "
               "read the aux's first u64 to discriminate root (SRV_REGISTRY_"
               "MAGIC) vs service (DEVSRV_SVC_MAGIC) vs conn (SRV_CONN_MAGIC)");

// The one immortal boot registry, mounted on kproc's /srv synthetic dir.
// Created by devsrv_init; resolved by the retained syscall path
// (srv_*_for_proc wrappers) + the in-kernel test harness. NULL before
// devsrv_init.
static struct SrvRegistry *g_boot_srv_registry;

static u64 g_srv_registry_created;
static u64 g_srv_registry_destroyed;

// =============================================================================
// Registry internals.
// =============================================================================

// Length-bounded name equality. Service names are NOT NUL-terminated in
// the registry — name_len is authoritative.
static bool srv_name_eq(const char *a, u8 alen, const char *b, u8 blen) {
    if (alen != blen) return false;
    for (u8 i = 0; i < alen; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Find a non-FREE entry by name in `reg`. PRECONDITION: caller holds
// reg->lock. Returns the entry (LIVE / RESERVING / TOMBSTONED) or NULL.
static struct SrvService *srv_find_locked(struct SrvRegistry *reg,
                                          const char *name, u8 name_len) {
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &reg->entries[i];
        if (e->state == SRV_STATE_FREE) continue;
        if (srv_name_eq(e->name, e->name_len, name, name_len)) return e;
    }
    return NULL;
}

// Wipe an entry back to FREE. PRECONDITION: caller holds the registry lock.
//
// `magic` is NOT cleared — a SrvService's magic is a permanent struct
// type tag (stamped once by srv_registry_create), never a liveness bit:
// `state` tracks FREE/RESERVING/LIVE/TOMBSTONED. A KObj_Srv service handle
// may outlive its entry's LIVE state, and handle_release_obj must still
// read SRV_SERVICE_MAGIC at offset 0 to discriminate it from a connection.
// `reg` (the back-pointer) is likewise permanent — never cleared.
//
// The backlog ARRAY is left as-is — callers (srv_abort on a RESERVING
// entry, which never reached LIVE so never enqueued a connection;
// srv_registry_drain after draining) guarantee no live SrvConn reference
// is dropped here. The accept Rendez is left untouched: sleep / wakeup
// own its `waiter` field, and clobbering it would strand a sleeper.
static void srv_clear_locked(struct SrvService *e) {
    e->state          = SRV_STATE_FREE;
    e->name_len       = 0;
    for (u32 i = 0; i < SRV_NAME_MAX; i++) e->name[i] = 0;
    e->poster_stripes = 0;
    e->poster_pid     = 0;
    e->backlog_head   = 0;
    e->backlog_tail   = 0;
    e->backlog_count  = 0;
}

// Push a connection onto an entry's accept backlog. PRECONDITION: caller
// holds the registry lock. The caller transfers ownership of one
// srvconn_ref to the backlog slot. Returns 0 on success, -1 if the entry
// is no longer LIVE (re-checked here atomically with the push) or the
// backlog is full.
static int srv_backlog_push_locked(struct SrvService *e, struct SrvConn *cn) {
    if (e->state != SRV_STATE_LIVE)                 return -1;
    if (e->backlog_count >= SRV_ACCEPT_BACKLOG)     return -1;
    e->backlog[e->backlog_head] = cn;
    e->backlog_head = (e->backlog_head + 1) % SRV_ACCEPT_BACKLOG;
    e->backlog_count++;
    return 0;
}

// Pop the oldest connection off an entry's accept backlog. PRECONDITION:
// caller holds the registry lock. Returns the SrvConn (ownership of its
// backlog ref passes to the caller) or NULL if the backlog is empty.
static struct SrvConn *srv_backlog_pop_locked(struct SrvService *e) {
    if (e->backlog_count == 0) return NULL;
    struct SrvConn *cn = e->backlog[e->backlog_tail];
    e->backlog[e->backlog_tail] = NULL;
    e->backlog_tail = (e->backlog_tail + 1) % SRV_ACCEPT_BACKLOG;
    e->backlog_count--;
    return cn;
}

// =============================================================================
// Registry lifecycle (stalk-3a).
// =============================================================================

// srv_registry_drain — tear down every pending connection in `reg`,
// wiping each entry to FREE, then wake any accept/poll waiter. Shared by
// srv_registry_reset (test harness, on the boot registry) and
// srv_registry_unref's last-drop. The teardown (srvconn_teardown /
// srvconn_unref take the SrvConn's locks; wakeup takes a Rendez lock) runs
// OUTSIDE reg->lock — the srvconn.c teardown discipline keeps those off
// any path that re-enters the registry lock.
static void srv_registry_drain(struct SrvRegistry *reg) {
    struct SrvConn *drained[SRV_MAX_SERVICES * SRV_ACCEPT_BACKLOG];
    u32             n_drained = 0;
    // Snapshot the per-entry accept Rendez + poll-list pointers BEFORE
    // clearing — a thread blocked in srv_accept_blocking or polling the
    // listener must be woken so it observes state != LIVE and returns.
    struct Rendez           *waked[SRV_MAX_SERVICES];
    struct poll_waiter_list *poll_waked[SRV_MAX_SERVICES];

    irq_state_t s = spin_lock_irqsave(&reg->lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &reg->entries[i];
        for (;;) {
            struct SrvConn *cn = srv_backlog_pop_locked(e);
            if (!cn) break;
            drained[n_drained++] = cn;
        }
        srv_clear_locked(e);
        waked[i]      = &e->accept_rendez;
        poll_waked[i] = &e->poll_list;
    }
    spin_unlock_irqrestore(&reg->lock, s);

    for (u32 i = 0; i < n_drained; i++) {
        srvconn_teardown(drained[i]);
        srvconn_unref(drained[i]);
    }
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        wakeup(waked[i]);
        poll_waiter_list_wake(poll_waked[i]);
    }
}

struct SrvRegistry *srv_registry_create(void) {
    struct SrvRegistry *reg = kmalloc(sizeof(*reg), KP_ZERO);
    if (!reg) return NULL;

    reg->magic = SRV_REGISTRY_MAGIC;
    spin_lock_init(&reg->lock);
    __atomic_store_n(&reg->ref, 1, __ATOMIC_RELAXED);

    // Stamp each entry's permanent type tag, reg back-pointer, and
    // poll-list (the list lock is the all-zero SPIN_LOCK_INIT from
    // KP_ZERO; the explicit init is the documented contract, mirroring
    // kernel/pipe.c::pipe_create). `state` is FREE (== 0) from KP_ZERO;
    // accept_rendez is {unlocked, no waiter} from KP_ZERO.
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        reg->entries[i].magic = SRV_SERVICE_MAGIC;
        reg->entries[i].reg   = reg;
        poll_waiter_list_init(&reg->entries[i].poll_list);
    }

    __atomic_fetch_add(&g_srv_registry_created, 1u, __ATOMIC_RELAXED);
    return reg;
}

void srv_registry_ref(struct SrvRegistry *reg) {
    if (!reg)                            extinction("srv_registry_ref(NULL)");
    if (reg->magic != SRV_REGISTRY_MAGIC)
        extinction("srv_registry_ref of corrupted registry");
    int pre = __atomic_fetch_add(&reg->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("srv_registry_ref of zero-ref registry (already freed?)");
}

void srv_registry_unref(struct SrvRegistry *reg) {
    if (!reg) return;                                // NULL-safe
    if (reg->magic != SRV_REGISTRY_MAGIC)
        extinction("srv_registry_unref of corrupted registry (use-after-free?)");
    int pre = __atomic_fetch_sub(&reg->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("srv_registry_unref of zero-ref registry");
    if (pre == 1) {
        // Last holder. Drain pending connections (the reset discipline)
        // then free. Clear magic before kfree so a stale-pointer read
        // fast-fails on the magic check (UAF defense, mirroring
        // spoor_free_internal).
        //
        // stalk-3b/A-5b ORDERING OBLIGATION (audit F2): kfree frees
        // entries[] too. A raw interior pointer into entries[] -- a
        // SrvService* held by a KObj_Srv listener handle's obj, or by an
        // in-flight srv_conn_open_in / svc_listener_poll -- carries NO
        // registry ref. In stalk-3a this never dangles (every such pointer
        // is into the immortal boot registry, which never reaches ref 0).
        // When 3b mints a MORTAL per-session registry, its last unref MUST
        // be ordered AFTER every listener/connection handle into it is
        // closed (the session poster is group-terminated first, #811,
        // closing its KObj_Srv listener) -- or the listener handle / the
        // devsrv_svc_ref must hold a registry ref that already covers it.
        srv_registry_drain(reg);
        reg->magic = 0;
        kfree(reg);
        __atomic_fetch_add(&g_srv_registry_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

struct SrvRegistry *srv_boot_registry(void) {
    return g_boot_srv_registry;
}

u64 srv_registry_total_created(void)   { return __atomic_load_n(&g_srv_registry_created, __ATOMIC_RELAXED); }
u64 srv_registry_total_destroyed(void) { return __atomic_load_n(&g_srv_registry_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// Registry API — the reserve / commit / abort two-phase post.
// =============================================================================

// srv_reserve_in — phase 1 against an explicit `reg`. The public
// srv_reserve binds the boot registry (the retained syscall path).
static int srv_reserve_in(struct SrvRegistry *reg,
                          const char *name, u8 name_len, struct Proc *poster,
                          enum srv_mode mode,
                          struct SrvService **svc_out, enum srv_state *prior_out) {
    if (!reg)                                              return -1;
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return -1;
    if (!svc_out || !prior_out)                            return -1;
    if (mode != SRV_MODE_9P && mode != SRV_MODE_BYTE)      return -1;
    /* F2 mode_change_on_rebind close (P6-pouch-sockets audit) is in the
     * TOMBSTONED branch below — see the same-mode check. */

    // proc_stripes fail-closes to 0 for a NULL / corrupted poster; a Proc
    // the kernel cannot identify cannot post a service (and 0 would alias
    // the fail-closed sentinel srv_proc_exit_notify matches against).
    u64 stripes = proc_stripes(poster);
    if (stripes == 0) return -1;

    irq_state_t s = spin_lock_irqsave(&reg->lock);

    struct SrvService *e = srv_find_locked(reg, name, name_len);
    if (e) {
        // The name exists. A LIVE or RESERVING entry must not be displaced
        // — no stealing a running or in-flight server. Only a TOMBSTONED
        // name (prior poster exited) is re-postable.
        if (e->state != SRV_STATE_TOMBSTONED) {
            spin_unlock_irqrestore(&reg->lock, s);
            return -1;
        }
        // F2 mode_change_on_rebind close (P6-pouch-sockets audit):
        // a service's mode is part of its identity — refuse a rebind
        // that flips the mode. Without this, a client A that captured
        // service_mode under the LIVE check (alongside poster_stripes)
        // and is about to mint a byte-mode SrvConn could observe a
        // 9P-mode rebound poster on the accept side, landing a wrong-
        // mode connection in the new poster's backlog. With this
        // check, a mode change requires a different service name.
        if (e->mode != mode) {
            spin_unlock_irqrestore(&reg->lock, s);
            return -1;
        }
        *prior_out = SRV_STATE_TOMBSTONED;
    } else {
        // Fresh post — claim a FREE slot.
        for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
            if (reg->entries[i].state == SRV_STATE_FREE) {
                e = &reg->entries[i];
                break;
            }
        }
        if (!e) {
            spin_unlock_irqrestore(&reg->lock, s);
            return -1;   // registry full
        }
        *prior_out = SRV_STATE_FREE;
    }

    // e->magic is already SRV_SERVICE_MAGIC + e->reg is already set —
    // srv_registry_create stamped every entry once; both are permanent.
    e->state          = SRV_STATE_RESERVING;
    e->name_len       = name_len;
    for (u8 i = 0; i < name_len; i++) e->name[i] = name[i];
    e->poster_stripes = stripes;
    e->poster_pid     = poster->pid;
    e->mode           = mode;
    *svc_out = e;

    spin_unlock_irqrestore(&reg->lock, s);
    return 0;
}

void srv_commit(struct SrvService *svc) {
    if (!svc)                          extinction("srv_commit(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_commit: bad service magic (wild pointer / corruption)");
    if (!svc->reg)                     extinction("srv_commit: service has no registry");

    irq_state_t s = spin_lock_irqsave(&svc->reg->lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_commit: entry not RESERVING (double commit / lifecycle bug)");
    svc->state = SRV_STATE_LIVE;
    spin_unlock_irqrestore(&svc->reg->lock, s);
}

void srv_abort(struct SrvService *svc, enum srv_state prior) {
    if (!svc)                          extinction("srv_abort(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_abort: bad service magic (wild pointer / corruption)");
    if (!svc->reg)                     extinction("srv_abort: service has no registry");
    if (prior != SRV_STATE_FREE && prior != SRV_STATE_TOMBSTONED)
        extinction("srv_abort: prior state must be FREE or TOMBSTONED");

    irq_state_t s = spin_lock_irqsave(&svc->reg->lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_abort: entry not RESERVING (lifecycle bug)");
    if (prior == SRV_STATE_FREE) {
        // A fresh post that failed leaves no trace. A RESERVING entry never
        // reached LIVE, so it never enqueued a connection — the backlog is
        // empty and srv_clear_locked is safe.
        srv_clear_locked(svc);
    } else {
        // A rebind that failed — restore the tombstone. The name stays
        // reserved; a tombstone carries no live poster identity.
        svc->state          = SRV_STATE_TOMBSTONED;
        svc->poster_stripes = 0;
        svc->poster_pid     = 0;
    }
    spin_unlock_irqrestore(&svc->reg->lock, s);
}

// devsrv_post_listener — the create=post path (stalk-3b, STALK-DESIGN.md
// §5.3 / D2). Mirrors sys_post_service_core's reserve -> handle_alloc(KObj_Srv)
// -> commit two-phase, but bound to the registry behind the /srv directory
// Spoor `root` (resolved from its aux) rather than the boot registry, and
// reached through SYS_WALK_CREATE on a /srv dir rather than the SYS_POST_-
// SERVICE syscall. The listener handle's obj is the registry entry; the entry
// outlives the handle (tombstoned at the poster's exit, never freed by handle
// close), so handle_release_obj's KOBJ_SRV case is a no-op for it.
int devsrv_post_listener(struct Proc *p, struct Spoor *root,
                         const char *name, size_t name_len, enum srv_mode mode) {
    if (!p)                                              return -1;
    if (!name)                                           return -1;
    if (name_len == 0 || name_len > SRV_NAME_MAX)        return -1;
    if (mode != SRV_MODE_9P && mode != SRV_MODE_BYTE)    return -1;

    // The parent MUST be a devsrv root Spoor whose aux is a SrvRegistry. The
    // caller (sys_walk_create_handler's devsrv branch) verified this; re-read
    // the aux defensively so a corrupt / non-root Spoor can never be coerced
    // into a registry pointer.
    if (!root || root->dc != 's' || !root->aux)          return -1;
    if (*(const u64 *)root->aux != SRV_REGISTRY_MAGIC)   return -1;
    struct SrvRegistry *reg = (struct SrvRegistry *)root->aux;

    // Post-gate — the SAME one-way joey-stamped bit SYS_POST_SERVICE checks
    // (CORVUS-DESIGN.md §6.1; corvus.tla PostService precondition). Fail-closed
    // for a bad Proc.
    if (!proc_may_post_service(p))                       return -1;

    // Service-name hygiene, identical to sys_post_service_core: printable
    // ASCII, no '/' (a /srv path separator), no control bytes -- so the name
    // can never be mis-parsed once it is a path component.
    for (size_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x21u || c > 0x7eu || c == '/')          return -1;
    }

    // Phase 1: reserve a slot in THIS registry (RESERVING; never observably
    // LIVE until the handle below exists).
    struct SrvService *svc = NULL;
    enum srv_state     prior = SRV_STATE_FREE;
    if (srv_reserve_in(reg, name, (u8)name_len, p, mode, &svc, &prior) != 0)
        return -1;

    // Install the KObj_Srv listener handle. handle_alloc does not take a
    // reference; the listener is non-transferable (handle_dup rejects KObj_Srv)
    // so it is pinned to p.
    hidx_t h = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, svc);
    if (h < 0) {
        // Phase 2 (failure): roll the reservation back to its prior state so a
        // retry -- or another poster -- can still claim the name.
        srv_abort(svc, prior);
        return -1;
    }

    // Phase 2 (success): RESERVING -> LIVE. Infallible.
    srv_commit(svc);
    return (int)h;
}

// srv_lookup_in — find a service by name in `reg`. Internal to the devsrv
// open=connect / walk paths; also the in-kernel test harness's name-based
// probe against the boot registry (srv_boot_registry()).
struct SrvService *srv_lookup_in(struct SrvRegistry *reg,
                                 const char *name, u8 name_len) {
    if (!reg)                                              return NULL;
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return NULL;
    irq_state_t s = spin_lock_irqsave(&reg->lock);
    struct SrvService *e = srv_find_locked(reg, name, name_len);
    spin_unlock_irqrestore(&reg->lock, s);
    return e;
}

// srv_proc_exit_notify_in — tombstone every LIVE service in `reg` posted
// by `p` (matched by stripes), draining each accept backlog. The public
// srv_proc_exit_notify binds the boot registry: in stalk-3a all posters
// post into the one boot registry (nothing migrates), so walking it is
// correct + complete. stalk-3b moves the tombstone trigger to the listener
// handle (which will carry its registry ref), making it per-registry for
// session registries — see STALK-DESIGN.md §5.1.
static void srv_proc_exit_notify_in(struct SrvRegistry *reg, struct Proc *p) {
    if (!reg) return;
    // proc_stripes fail-closes to 0; a LIVE poster always carries a
    // non-zero stripes tag, so a 0 here matches nothing.
    u64 stripes = proc_stripes(p);
    if (stripes == 0) return;

    // Collect this poster's pending connections + tombstoned services so
    // the heavy work (srvconn_teardown / srvconn_unref take the SrvConn's
    // own locks; wakeup takes a Rendez lock) runs OUTSIDE the registry
    // lock — the srvconn.c teardown discipline keeps those off any path
    // that re-enters the registry lock. A Proc may post more than one
    // service (test_devsrv post_basic posts two), so every matching entry
    // is tombstoned, not just the first.
    struct SrvConn    *drained[SRV_MAX_SERVICES * SRV_ACCEPT_BACKLOG];
    u32                n_drained = 0;
    struct SrvService *tombed[SRV_MAX_SERVICES];
    u32                n_tombed = 0;

    irq_state_t s = spin_lock_irqsave(&reg->lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &reg->entries[i];
        if (e->state == SRV_STATE_LIVE && e->poster_stripes == stripes) {
            // Tombstone: the name stays reserved (re-postable only by a
            // joey-marked Proc — CORVUS-DESIGN.md §6.1), the dead poster
            // identity is cleared so a tombstone carries no live poster.
            e->state          = SRV_STATE_TOMBSTONED;
            e->poster_stripes = 0;
            e->poster_pid     = 0;
            // Drain the accept backlog: no live server remains to accept
            // these connections, so each is torn down (its client wakes
            // with EOF rather than hanging on a dead server).
            for (;;) {
                struct SrvConn *cn = srv_backlog_pop_locked(e);
                if (!cn) break;
                drained[n_drained++] = cn;
            }
            tombed[n_tombed++] = e;
        }
    }
    spin_unlock_irqrestore(&reg->lock, s);

    for (u32 i = 0; i < n_drained; i++) {
        srvconn_teardown(drained[i]);   // EOF both rings — wakes the client
        srvconn_unref(drained[i]);      // drop the backlog reference
    }
    // Wake any thread blocked accepting on a tombstoned service — it sees
    // state != LIVE and srv_accept_blocking returns NULL. Defensive: the
    // exiting poster is normally its service's only accepter.
    //
    // Also wake every listener poller (P5-poll-b): a tombstone surfaces
    // POLLHUP on the listener. specs/poll.tla MakeReady.
    for (u32 i = 0; i < n_tombed; i++) {
        wakeup(&tombed[i]->accept_rendez);
        poll_waiter_list_wake(&tombed[i]->poll_list);
    }
}

void srv_proc_exit_notify(struct Proc *p) {
    srv_proc_exit_notify_in(g_boot_srv_registry, p);
}

static int srv_registry_count_in(struct SrvRegistry *reg) {
    if (!reg) return 0;
    int n = 0;
    irq_state_t s = spin_lock_irqsave(&reg->lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        if (reg->entries[i].state != SRV_STATE_FREE) n++;
    }
    spin_unlock_irqrestore(&reg->lock, s);
    return n;
}

int srv_registry_count(void) {
    return srv_registry_count_in(g_boot_srv_registry);
}

int srv_backlog_depth(struct SrvService *svc) {
    if (!svc || svc->magic != SRV_SERVICE_MAGIC || !svc->reg) return -1;
    irq_state_t s = spin_lock_irqsave(&svc->reg->lock);
    int n = (int)svc->backlog_count;
    spin_unlock_irqrestore(&svc->reg->lock, s);
    return n;
}

// srv_registry_reset — TEST SUPPORT ONLY. Drain the BOOT registry to
// all-FREE, tearing down every pending connection first. Deliberately NOT
// declared in devsrv.h: no production caller exists; the in-kernel test
// harness extern-declares it so each devsrv test starts from an empty boot
// registry (the registry its public-API calls resolve).
void srv_registry_reset(void) {
    if (!g_boot_srv_registry) return;
    srv_registry_drain(g_boot_srv_registry);
}

// =============================================================================
// Per-connection layer (P5-corvus-srv-impl-a3b).
// =============================================================================

// accept_cond_is_ready — sleep()'s wait predicate for srv_accept_blocking.
// Reads backlog_count / state WITHOUT the registry lock: sleep evaluates
// it under the accept Rendez lock, and every producer (srv_conn_open_in's
// push, srv_proc_exit_notify_in's tombstone) mutates these fields under
// the registry lock (svc->reg->lock) and then calls wakeup(), whose
// Rendez-lock acquisition provides the happens-before. The discipline
// matches srvconn.c's chan_cond_readable.
static int accept_cond_is_ready(void *arg) {
    struct SrvService *svc = (struct SrvService *)arg;
    return (svc->backlog_count > 0) || (svc->state != SRV_STATE_LIVE);
}

struct SrvConn *srv_accept_blocking(struct SrvService *svc) {
    if (!svc || svc->magic != SRV_SERVICE_MAGIC || !svc->reg) return NULL;

    for (;;) {
        irq_state_t s = spin_lock_irqsave(&svc->reg->lock);
        struct SrvConn *cn  = srv_backlog_pop_locked(svc);
        enum srv_state  st  = svc->state;
        spin_unlock_irqrestore(&svc->reg->lock, s);

        if (cn) return cn;                 // dequeued — ownership to caller
        if (st != SRV_STATE_LIVE) return NULL;   // service gone; give up

        // Empty + LIVE: block until a client opens (a wakeup from the
        // push) or the service stops being LIVE (a wakeup from the
        // tombstone). sleep re-checks accept_cond under the Rendez lock —
        // a wakeup between the unlock above and the sleep transition is
        // not lost (specs/scheduler.tla NoMissedWakeup).
        // #811 (ARCH §8.8.1): death-interrupted -> Proc group-terminating;
        // return so the Thread unwinds to its EL0-return die-check.
        if (sleep(&svc->accept_rendez, accept_cond_is_ready, svc) == SLEEP_INTR)
            return NULL;
    }
}

struct Spoor *devsrv_make_conn_spoor(struct SrvConn *cn) {
    if (!cn) return NULL;
    struct Spoor *c = spoor_alloc(&devsrv);
    if (!c) return NULL;
    // The SrvConn itself is the Spoor's aux — SRV_CONN_MAGIC at its
    // offset 0 self-identifies it (devsrv_close discriminates on it). The
    // caller's SrvConn reference becomes the Spoor's; devsrv_close drops
    // it. An accepted connection is pre-opened — corvus reads Tmsg / writes
    // Rmsg on it directly through SYS_READ / SYS_WRITE.
    c->aux      = cn;
    c->qid.type = QTFILE;
    c->qid.path = 0;
    c->qid.vers = 0;
    c->flag    |= COPEN;
    c->mode     = 2;            // ORDWR
    c->offset   = 0;
    return c;
}

// =============================================================================
// The devsrv Dev.
// =============================================================================

static void devsrv_reset(void)    { /* no-op */ }

// init — create the one immortal boot registry (stalk-3a). srv_registry_
// create stamps every entry's permanent magic + reg back-pointer +
// poll_list. dev_init calls this once at boot, before joey's kproc bringup
// mounts the boot registry's devsrv root on /srv.
static void devsrv_init(void) {
    if (g_boot_srv_registry) extinction("devsrv_init called twice");
    g_boot_srv_registry = srv_registry_create();
    if (!g_boot_srv_registry)
        extinction("devsrv_init: srv_registry_create(boot) failed");
}

static void devsrv_shutdown(void) { /* no-op */ }

// attach — produce a /srv root directory Spoor over the BOOT registry. A
// direct `#s` attach (vs. the mount, which uses devsrv_attach_registry
// with an explicit registry) yields the one namespace-resident registry at
// v1.0. `spec` is unused. NULL before devsrv_init (no boot registry yet).
static struct Spoor *devsrv_attach(const char *spec) {
    (void)spec;
    return devsrv_attach_registry(g_boot_srv_registry);
}

// devsrv_attach_registry — mint a /srv root directory Spoor whose aux is
// `reg` (taking one registry ref). The mount source at boot + per session
// (A-5b-body). spoor_clunk of the returned root (on its last holder) runs
// devsrv_close, which drops the registry ref.
struct Spoor *devsrv_attach_registry(struct SrvRegistry *reg) {
    if (!reg) return NULL;
    struct Spoor *c = spoor_alloc(&devsrv);
    if (!c) return NULL;
    c->aux      = reg;
    c->qid.type = QTDIR;
    c->qid.path = 0;
    c->qid.vers = 0;
    // stalk-3a is the point devsrv becomes a MULTI-instance Dev: each
    // registry is a distinct instance (the boot registry; a future per-
    // session login registry). Stamp a per-instance devno (Plan 9
    // Chan.dev) like dev9p_attach_client, so two registry roots have
    // distinct mount-key identity (dc, devno, qid.path) -- without it both
    // are (s,0,0) and stalk-2's (dc,devno,qid.path) keying / the mount
    // cycle check could not tell them apart (audit F1). A walk/clone of
    // this root inherits the devno (spoor_clone copies it).
    c->devno    = spoor_next_devno();
    srv_registry_ref(reg);     // the root Spoor instance holds one registry ref
    return c;
}

// devsrv_conn_of — the SrvConn behind a connection Spoor, or NULL if `c`
// is not one. A devsrv Spoor's aux discriminates its flavor: a SrvRegistry
// (SRV_REGISTRY_MAGIC) for a /srv root, a devsrv_svc_ref (DEVSRV_SVC_MAGIC)
// for a service Spoor, or a SrvConn (SRV_CONN_MAGIC) for an accepted
// connection. Non-static — SYS_SRV_PEER (kernel/syscall.c) resolves a
// connection endpoint handle through here; declared in <thylacine/devsrv.h>.
struct SrvConn *devsrv_conn_of(struct Spoor *c) {
    if (!c || c->dc != 's' || !c->aux)          return NULL;
    if (*(const u64 *)c->aux != SRV_CONN_MAGIC) return NULL;
    return (struct SrvConn *)c->aux;
}

// walk — only a /srv root walks (aux = the SrvRegistry, SRV_REGISTRY_MAGIC),
// and only one component deep: a walk of /srv/<name> yields a service Spoor
// naming a LIVE posted service in THIS root's registry. The service Spoor
// is structurally KObj_Srv (it carries devsrv's dc='s'). Walking out of a
// service or connection Spoor is rejected.
//
// Registry-ref discipline (stalk-3a): `nc` is a spoor_clone of the root, so
// spoor_clone copied nc->aux = reg — but a clone holds NO registry ref.
// We NORMALIZE nc->aux = NULL on entry, then take the registry ref + set
// nc->aux only on the success path. A walk failure therefore leaves
// nc->aux == NULL, and devsrv_close(nc) is a clean no-op — no phantom
// srv_registry_unref of a ref nc never took. (clone_walk_zero's failure
// path does `nc->aux = NULL; spoor_unref` — no Dev close — so a ref taken
// on a failing nname==0 path would leak; we drop it ourselves below.)
static struct Walkqid *devsrv_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c || c->dc != 's' || !c->aux)                 return NULL;
    if (*(const u64 *)c->aux != SRV_REGISTRY_MAGIC)    return NULL;   // root only
    if (!nc || nname < 0)                              return NULL;

    struct SrvRegistry *reg = (struct SrvRegistry *)c->aux;
    nc->aux = NULL;            // normalize the clone-copied reg (see header)

    if (nname == 0) {
        // Clone — nc is the caller's shallow copy of the root. It becomes a
        // NEW root instance naming the same registry, so it takes its OWN
        // registry ref (mirroring dev9p's attached_owner clone-bump);
        // devsrv_close(nc) drops it.
        srv_registry_ref(reg);
        struct Walkqid *w = walkqid_alloc(1);
        if (!w) {
            srv_registry_unref(reg);   // drop the ref we just took (no leak)
            return NULL;               // nc->aux == NULL -> clean clone_walk_zero unref
        }
        nc->aux  = reg;                // commit: nc is a live root instance
        w->nqid  = 0;
        w->spoor = nc;
        return w;
    }
    if (nname != 1) return NULL;        // no /srv/<name>/<path> nesting yet

    // name[i] is NUL-terminated (the Dev walk contract); the explicit cap
    // bounds the scan.
    const char *s = name[0];
    if (!s) return NULL;
    u32 len = 0;
    while (len < SRV_NAME_MAX && s[len] != '\0') len++;
    if (len == 0 || s[len] != '\0') return NULL;   // empty or over-long

    struct SrvService *svc = srv_lookup_in(reg, s, (u8)len);
    if (!svc) return NULL;
    {
        irq_state_t st = spin_lock_irqsave(&reg->lock);
        bool live = (svc->state == SRV_STATE_LIVE);
        spin_unlock_irqrestore(&reg->lock, st);
        if (!live) return NULL;        // only a LIVE service is walkable
    }

    struct devsrv_svc_ref *ref = kmalloc(sizeof(*ref), KP_ZERO);
    if (!ref) return NULL;
    ref->magic    = DEVSRV_SVC_MAGIC;
    ref->name_len = (u8)len;
    for (u32 i = 0; i < len; i++) ref->name[i] = s[i];
    ref->reg      = reg;
    srv_registry_ref(reg);             // the service-ref carries one registry ref

    struct Walkqid *w = walkqid_alloc(1);
    if (!w) {
        srv_registry_unref(reg);       // drop the svc-ref's ref (no leak)
        kfree(ref);
        return NULL;                   // nc->aux == NULL -> clean
    }

    nc->aux      = ref;        // commit: nc is a service-ref Spoor
    nc->qid.type = QTFILE;
    nc->qid.path = 0;
    nc->qid.vers = 0;
    w->nqid    = 1;
    w->qid[0]  = nc->qid;
    w->spoor   = nc;
    return w;
}

static int devsrv_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// devsrv_open_connect — the connect core (stalk-3b-β; STALK-DESIGN.md §5.2 / D5).
// Testable with an explicit Proc; the Dev.open vtable slot (devsrv_open) calls it
// with current_thread()->proc. `c` is a /srv/<name> service-ref Spoor
// (devsrv_walk's product, DEVSRV_SVC_MAGIC aux). Resolve the service in c's
// registry, mint a SrvConn bound to `p`'s kernel-stamped identity, enqueue it on
// the poster's accept backlog (waking the poster), and return the connection
// ENDPOINT as a KOBJ_SPOOR Spoor:
//   - 9p-mode (corvus): wrap the SrvConn's CLIENT side into a dev9p root Spoor
//     (Tversion + Tattach via srvconn_attach_dev9p_root) -- the two-step attach
//     (the client then opens "ctl" relative to this root). The client gets no
//     conn Spoor, so the create ref is dropped here (the dev9p root + the
//     poster's backlog ref own the SrvConn).
//   - byte-mode (stratum-fs / pouch socket): return a CLIENT-direction byte-conn
//     Spoor (CSRVCLIENT). The create ref becomes the Spoor's ref.
// devsrv (dc='s') is NOT perm_enforced, so stalk runs no rwx gate on the service
// node; the per-territory /srv visibility (the mount table) is the isolation
// boundary (I-1).
struct Spoor *devsrv_open_connect(struct Proc *p, struct Spoor *c, int omode) {
    (void)omode;
    if (!p)                                        return NULL;
    if (!c || c->dc != 's' || !c->aux)             return NULL;
    if (*(const u64 *)c->aux != DEVSRV_SVC_MAGIC)  return NULL;
    struct devsrv_svc_ref *ref = (struct devsrv_svc_ref *)c->aux;
    struct SrvRegistry    *reg = ref->reg;
    if (!reg)                                      return NULL;

    // Global live-connection cap (soft; the per-service backlog is the hard
    // bound). The per-Proc cap was removed (stalk-3b-β / 3a-audit F4): a session
    // needs corvus AND its stratum-fs concurrently.
    if (srvconn_total_created() - srvconn_total_freed() >= SRV_MAX_CONNS)
        return NULL;

    // Resolve the service; capture poster stripes + transport mode under the
    // registry lock, atomically with the LIVE check (both immutable while LIVE).
    struct SrvService *svc = srv_lookup_in(reg, ref->name, ref->name_len);
    if (!svc) return NULL;
    u64           poster_stripes;
    enum srv_mode service_mode;
    {
        irq_state_t ls = spin_lock_irqsave(&reg->lock);
        bool live      = (svc->state == SRV_STATE_LIVE);
        poster_stripes = svc->poster_stripes;
        service_mode   = svc->mode;
        spin_unlock_irqrestore(&reg->lock, ls);
        if (!live) return NULL;
    }

    // Mint the connection (peer + server identity captured BY VALUE -- no raw
    // Proc* / SrvService* held, so neither a peer exit nor a tombstone-then-
    // rebind turns a later read into a UAF). create ref == 1.
    struct SrvConn *cn = srvconn_create(proc_stripes(p), p->pid,
                                        proc_is_console_attached(p), poster_stripes);
    if (!cn) return NULL;
    if (service_mode == SRV_MODE_BYTE) srvconn_set_byte_mode(cn);

    // A 2nd ref for the accept-backlog slot; the push re-validates LIVE atomically.
    srvconn_ref(cn);
    irq_state_t s = spin_lock_irqsave(&reg->lock);
    int rc = srv_backlog_push_locked(svc, cn);
    spin_unlock_irqrestore(&reg->lock, s);
    if (rc != 0) {
        srvconn_unref(cn);     // drop the backlog ref
        srvconn_unref(cn);     // drop the create ref -> teardown + free
        return NULL;
    }
    // Wake a poster blocked in SYS_SRV_ACCEPT + any listener poller (the push
    // committed under reg->lock; the wakes run after release -- chan_produce
    // discipline, specs/poll.tla MakeReady).
    wakeup(&svc->accept_rendez);
    poll_waiter_list_wake(&svc->poll_list);

    if (service_mode == SRV_MODE_BYTE) {
        // Byte-mode endpoint: a CLIENT-direction conn Spoor. devsrv_make_conn_spoor
        // takes the create ref (it becomes the Spoor's ref; devsrv_close drops it).
        // The only v1.0 byte client (joey -> stratum-fs) immediately SYS_ATTACH_
        // 9P_SRV-wraps this Spoor; the direct client read/write path is first used
        // by pouch (3c) -- the CSRVCLIENT direction is set now regardless.
        struct Spoor *cs = devsrv_make_conn_spoor(cn);
        if (!cs) {
            srvconn_teardown(cn);  // the poster's accept sees EOF
            srvconn_unref(cn);     // drop the create ref; the poster drains the backlog ref
            return NULL;
        }
        cs->flag |= CSRVCLIENT;    // client endpoint: read s2c / write c2s
        return cs;
    }

    // 9p-mode endpoint: wrap the SrvConn's CLIENT side into a dev9p root Spoor.
    // The blocking handshake runs lock-free here; the poster (corvus), woken
    // above, accepts + responds concurrently. On any failure the conn is torn
    // down so the poster's accept sees a dead conn.
    int err = 0;
    struct Spoor *root = srvconn_attach_dev9p_root(cn, NULL, 0, p->principal_id, &err);
    if (!root) {
        srvconn_teardown(cn);      // idempotent (the helper may already have torn it)
        srvconn_unref(cn);         // drop the create ref; the poster drains the backlog ref
        return NULL;
    }
    srvconn_unref(cn);             // drop the create ref; root owns the session, the poster the backlog ref
    return root;
}

// open — the Dev.open vtable slot. A Dev.open takes no Proc, so resolve the
// connecting Proc from the running Thread (stalk runs in syscall context on the
// caller's Thread). stalk-3b-β; STALK-DESIGN.md §5.2.
static struct Spoor *devsrv_open(struct Spoor *c, int omode) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return NULL;
    return devsrv_open_connect(t->proc, c, omode);
}

// create — a graceful-fail stub. create=post does NOT ride this Spoor-
// returning vtable slot: a post yields a KObj_Srv listener handle (not a
// Spoor), so sys_walk_create_handler's devsrv branch calls devsrv_post_
// listener directly (perm & DMSRVBYTE selects byte- vs 9P-mode) and never
// reaches here. stalk-3c retired the name-only SYS_POST_SERVICE; posting is
// SYS_WALK_CREATE on a /srv dir (STALK-DESIGN.md §5.3).
static struct Spoor *devsrv_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

// close — release per-Spoor state. A /srv root holds a SrvRegistry ref; a
// service Spoor holds a kmalloc'd devsrv_svc_ref (carrying a registry ref);
// a connection Spoor holds a SrvConn reference. Closing a connection Spoor
// is a connection close (CORVUS-DESIGN.md §6.2): tear the connection down
// so the peer wakes, then release the reference. A Spoor with aux == NULL
// (a failed/transient walk clone normalized in devsrv_walk) is a clean
// no-op.
static void devsrv_close(struct Spoor *c) {
    if (!c || c->dc != 's' || !c->aux) return;   // root sans-reg / transient — no-op
    u64 m = *(const u64 *)c->aux;
    if (m == SRV_REGISTRY_MAGIC) {
        // A /srv root instance: drop its registry ref (the last drop drains
        // + frees the registry; the boot registry never reaches that — its
        // mount holds a ref forever).
        srv_registry_unref((struct SrvRegistry *)c->aux);
    } else if (m == SRV_CONN_MAGIC) {
        // Closing a connection Spoor is a connection close (CORVUS-DESIGN §6.2):
        // tear the connection down so the peer wakes, then drop the reference.
        //
        // The kernel-attached SKIP is for the CLIENT endpoint ONLY. When the
        // kernel wraps a conn's rings in a 9P client (SYS_ATTACH_9P_SRV, or
        // devsrv_open's 9p-mode path), those c2s/s2c rings are LOAD-BEARING for
        // that client, so a userspace t_close of the now-redundant CLIENT conn
        // endpoint must NOT EOF them -- teardown migrates to the adapter's
        // transport.close at p9_attached_destroy (the last holder). Mirrors
        // handle.c's KOBJ_SRV release arm (16c-integration); stalk-3b-β makes the
        // CLIENT endpoint a CSRVCLIENT conn Spoor that releases through HERE.
        //
        // #841: the SERVER endpoint (corvus's accepted Spoor -- NO CSRVCLIENT)
        // is the OTHER side of the same shared SrvConn, and its kernel_attached
        // flag was set by the CLIENT's attach. But the server closing means the
        // 9P server is GONE -- it MUST EOF the rings so the kernel client's
        // blocked recv wakes with EOF. A no-timeout client (ARCH §21.10) observes
        // connection death via EOF, never a per-op timeout; honoring
        // kernel_attached on the server side suppressed the EOF and hung the
        // client's Tclunk forever (the 30s per-op deadline the #841 restoration
        // removed had been masking exactly this -- corvus's post-BadFormat Q11
        // teardown). So skip teardown ONLY for the kernel-attached CLIENT side.
        struct SrvConn *cn = (struct SrvConn *)c->aux;
        bool client_ep = (c->flag & CSRVCLIENT) != 0;
        if (!(client_ep && srvconn_is_kernel_attached(cn))) srvconn_teardown(cn);
        srvconn_unref(cn);
    } else if (m == DEVSRV_SVC_MAGIC) {
        struct devsrv_svc_ref *ref = (struct devsrv_svc_ref *)c->aux;
        struct SrvRegistry    *r   = ref->reg;
        ref->magic = 0;
        kfree(ref);
        srv_registry_unref(r);     // drop the svc-ref's registry ref
    } else {
        extinction("devsrv_close: Spoor aux has unknown magic (corruption)");
    }
    c->aux = NULL;
}

// read — a connection Spoor's read drains the c2s ring (the bytes the
// kernel 9P client sent toward corvus, or that the pouch AF_UNIX
// SOCK_STREAM client sent toward its peer).
//
// 9P mode (corvus / stratumd): NON-BLOCKING — returns 0 on empty-but-
// live. corvus polls-then-reads (t_poll on the listener + per-conn
// POLLIN before pulling 9P frames off c2s).
//
// Byte mode (pouch AF_UNIX SOCK_STREAM, P6-pouch-sockets sub-chunk 12):
// BLOCKING — POSIX stream-socket read semantics. F1 close of the
// pouch-sockets audit: the non-blocking variant returned 0 to a POSIX
// server racing the client's first write across SMP CPUs, which the
// userspace test interpreted as EOF and failed. Routed through
// srvconn_server_recv_blocking for byte-mode SrvConns.
//
// EOF (return 0 on torn-down conn) is identical in both modes; -1 is
// bad args / wrong magic. `off` is ignored — a connection is a byte
// stream. The /srv root and service Spoors are not readable.
static long devsrv_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    struct SrvConn *cn = devsrv_conn_of(c);
    if (!cn) return -1;
    if (c->flag & CSRVCLIENT) {
        // stalk-3b-E F1: refuse direct I/O on a kernel-attached conn endpoint.
        // After SYS_ATTACH_9P_SRV wraps this byte-conn Spoor, the c2s/s2c rings
        // are load-bearing for the kernel 9P client's Twalk/Tread/Twrite stream;
        // a userspace read here would drain Rread/Rwalk bytes meant for that
        // client and corrupt the session. This is the same hazard the KOBJ_SRV
        // r/w arms close (syscall.c R1 F3); the endpoint moved KObj_Srv ->
        // KOBJ_SPOOR (CSRVCLIENT) in stalk-3b-β-C1, so the I/O guard must too.
        if (srvconn_is_kernel_attached(cn)) return -1;
        // CLIENT endpoint (a byte-mode connect's Spoor, stalk-3b-β): read the
        // server's replies off s2c. POSIX blocking-read semantics (deadline 0 =
        // block until data or EOF). First exercised by the pouch AF_UNIX client
        // (3c) -- the only v1.0 byte client (joey -> stratum-fs) attach-wraps its
        // conn Spoor, so the kernel 9P client drives s2c via srvconn_client_recv
        // directly. The CSRVCLIENT direction is the mirror of the server arm.
        return srvconn_client_recv(cn, (u8 *)buf, n);
    }
    // SERVER endpoint (the poster's accepted Spoor): read c2s. Atomic acquire on
    // byte_mode (F5 close): pair the setter's ATOMIC_RELEASE in
    // srvconn_set_byte_mode so a multi-thread Proc reading this endpoint sees
    // the mode that was propagated at SrvConn mint.
    bool bm = __atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE);
    if (bm) {
        return srvconn_server_recv_blocking(cn, (u8 *)buf, n);
    }
    return srvconn_server_recv(cn, (u8 *)buf, n);
}

static struct Block *devsrv_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// write — a connection Spoor's write fills the s2c ring (corvus's Rmsg
// bytes toward the kernel 9P client). Returns bytes accepted or -1 if the
// connection is torn down. The /srv root and service Spoors are not
// writable.
static long devsrv_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    struct SrvConn *cn = devsrv_conn_of(c);
    if (!cn) return -1;
    if (c->flag & CSRVCLIENT) {
        // stalk-3b-E F1: refuse direct I/O on a kernel-attached conn endpoint
        // (see devsrv_read) -- a userspace write would interleave bytes into the
        // c2s ring out-of-band with the kernel 9P client's request stream.
        if (srvconn_is_kernel_attached(cn)) return -1;
        // CLIENT endpoint: write toward the server via c2s (non-blocking). The
        // mirror of the server arm; stalk-3b-β (see devsrv_read).
        return srvconn_client_send(cn, (const u8 *)buf, n);
    }
    // SERVER endpoint: write replies toward the client via s2c.
    return srvconn_server_send(cn, (const u8 *)buf, n);
}

static long devsrv_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devsrv_remove(struct Spoor *c) {
    (void)c;
    // no-op
}

static int devsrv_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devsrv_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

// =============================================================================
// poll — readiness probe (P5-poll-b).
// =============================================================================
//
// devsrv_poll (the Dev vtable slot) routes a connection Spoor's poll to
// srvconn_poll; the root / service-ref Spoors are not readable directly,
// so they report no readiness state (return 0 — POSIX-equivalent to "this
// fd never has data" so a poller with timeout will block until timeout).
// The corvus listener-poll path goes through srv_handle_poll, NOT here:
// the listener is a KObj_Srv handle whose obj is a SrvService, not a
// Spoor — poll.c's KObj_Srv branch resolves it through srv_handle_poll.

// svc_listener_poll — listener-readiness probe on a SrvService. POLLIN
// when the accept backlog has one or more waiting connections (corvus
// has something to accept); POLLHUP when the service is no longer LIVE
// (tombstoned by srv_proc_exit_notify or wiped by srv_registry_drain).
//
// The sample-and-register are both atomic under the registry lock
// (svc->reg->lock); the producer side (srv_conn_open_in →
// srv_backlog_push_locked under the registry lock, then poll_waiter_list_
// wake after release; srv_proc_exit_notify_in under the registry lock,
// then poll_waiter_list_wake after release) commits the readiness change
// under the same lock.
static short svc_listener_poll(struct SrvService *svc, short events,
                               struct poll_waiter *pw) {
    if (!svc) return POLLERR;
    if (svc->magic != SRV_SERVICE_MAGIC) {
        extinction("svc_listener_poll: bad service magic (UAF / wild ptr?)");
    }
    if (!svc->reg) {
        extinction("svc_listener_poll: service has no registry");
    }

    short revents = 0;

    irq_state_t s = spin_lock_irqsave(&svc->reg->lock);
    if (svc->backlog_count > 0)        revents |= POLLIN;
    if (svc->state != SRV_STATE_LIVE)  revents |= POLLHUP;
    if (pw) {
        poll_waiter_list_register(&svc->poll_list, pw);
    }
    spin_unlock_irqrestore(&svc->reg->lock, s);

    // POSIX: POLLIN/POLLOUT only when requested; POLLHUP always. POLLOUT
    // is undefined for a listener — never set.
    return (short)(revents & (events | POLL_OUTPUT_ONLY));
}

short srv_handle_poll(void *obj, short events, struct poll_waiter *pw) {
    (void)events; (void)pw;
    if (!obj) return POLLNVAL;
    // The first u64 of a KObj_Srv obj is its struct's magic word — see
    // SRV_SERVICE_MAGIC / SRV_CONN_MAGIC. Read once: a torn UAF would
    // observe 0 here (the freed-object scrubs clear magic before kfree).
    u64 magic = *(const u64 *)obj;
    if (magic == SRV_SERVICE_MAGIC) {
        return svc_listener_poll((struct SrvService *)obj, events, pw);
    }
    if (magic == SRV_CONN_MAGIC) {
        // Client-side connection handle. `srvconn_poll`'s semantics are
        // SERVER-endpoint (POLLIN ↔ c2s.count > 0 — bytes corvus reads;
        // POLLOUT ↔ s2c room — room corvus writes). A client polling its
        // OWN handle expects mirror-image semantics (POLLIN ↔ s2c has a
        // reply; POLLOUT ↔ c2s has room) — delegating here would return
        // misleading revents. No v1.0 caller polls this (joey drives 9P
        // synchronously through the kernel client; corvus polls its
        // accept-side Spoor, not its KObj_Srv). Fail-closed until the
        // client-side poll story lands (its own SrvConn `client_poll`
        // entry with mirrored semantics + a separate hook list).
        return POLLNVAL;
    }
    // Unknown magic — corruption, or a future KObj_Srv flavor.
    return POLLNVAL;
}

// devsrv_poll — the Dev `.poll` slot. Dispatches by Spoor flavor (root /
// service-ref / connection — discriminated by the aux's first u64; see
// devsrv_conn_of's comment).
static short devsrv_poll(struct Spoor *c, short events,
                         struct poll_waiter *pw) {
    if (!c || c->dc != 's') return POLLERR;
    if (!c->aux) {
        // A transient/failed walk clone (aux normalized to NULL). No
        // readiness state; the caller's poll blocks until timeout.
        return 0;
    }
    u64 m = *(const u64 *)c->aux;
    if (m == SRV_CONN_MAGIC) {
        return srvconn_poll((struct SrvConn *)c->aux, events, pw);
    }
    if (m == SRV_REGISTRY_MAGIC) {
        // /srv root Spoor — no readiness state. The caller is asking about
        // a directory; a poll on a directory has no real meaning here.
        // Report no events (the caller's poll blocks until timeout).
        return 0;
    }
    if (m == DEVSRV_SVC_MAGIC) {
        // Service-ref Spoor — the result of walking /srv/<name>. Used by
        // the client-open path (stalk-3b) to mint a SrvConn; not a pollable
        // transport itself. No events.
        return 0;
    }
    extinction("devsrv_poll: Spoor aux has unknown magic (corruption)");
    return POLLERR;   // unreachable
}

struct Dev devsrv = {
    .dc       = 's',          // Plan 9 #s
    .name     = "srv",

    .reset    = devsrv_reset,
    .init     = devsrv_init,
    .shutdown = devsrv_shutdown,

    .attach   = devsrv_attach,
    .walk     = devsrv_walk,
    .stat     = devsrv_stat,

    .open     = devsrv_open,
    .create   = devsrv_create,
    .close    = devsrv_close,

    .read     = devsrv_read,
    .bread    = devsrv_bread,
    .write    = devsrv_write,
    .bwrite   = devsrv_bwrite,

    .poll     = devsrv_poll,        // P5-poll-b

    .remove   = devsrv_remove,
    .wstat    = devsrv_wstat,
    .power    = devsrv_power,
};
