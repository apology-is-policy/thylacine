// devsrv — the /srv service registry Dev (P5-corvus-srv-impl-a2).
//
// Per ARCHITECTURE.md §9.4 + CORVUS-DESIGN.md §6.1. `/srv` is a kernel
// Dev (Plan 9's `#s`) by which a userspace 9P server registers a name
// with SYS_POST_SERVICE; the kernel mediates per-connection client
// access. `devsrv` is deliberately distinct from `dev9p` so a Spoor
// walked out of `/srv` is structurally a KObj_Srv kernel object —
// non-transferable, which keeps the kernel-stamped peer identity behind
// it unforgeable.
//
// This chunk lands the service registry + the devsrv Dev's attach. The
// post path (SYS_POST_SERVICE) lives in kernel/syscall.c; this file owns
// the registry it commits into. The devsrv walk op (a walk of /srv/<name>
// minting a per-connection KObj_Srv Spoor) is P5-corvus-srv-impl-a3.
//
// Spec: specs/corvus.tla — MarkMayPost / PostService / ServiceTombstone,
// invariant ServicePosterEverMarked.

#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// The registry. Static storage zero-initializes every entry's `state` to
// SRV_STATE_FREE (== 0) and the lock to the all-zero SPIN_LOCK_INIT form.
struct SrvRegistry {
    spin_lock_t       lock;
    struct SrvService entries[SRV_MAX_SERVICES];
};
static struct SrvRegistry g_srv_registry;

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

// Find a non-FREE entry by name. PRECONDITION: caller holds the registry
// lock. Returns the entry (LIVE / RESERVING / TOMBSTONED) or NULL.
static struct SrvService *srv_find_locked(const char *name, u8 name_len) {
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &g_srv_registry.entries[i];
        if (e->state == SRV_STATE_FREE) continue;
        if (srv_name_eq(e->name, e->name_len, name, name_len)) return e;
    }
    return NULL;
}

// Wipe an entry back to FREE. PRECONDITION: caller holds the registry lock.
static void srv_clear_locked(struct SrvService *e) {
    e->magic          = 0;
    e->state          = SRV_STATE_FREE;
    e->name_len       = 0;
    for (u32 i = 0; i < SRV_NAME_MAX; i++) e->name[i] = 0;
    e->poster_stripes = 0;
    e->poster_pid     = 0;
}

// =============================================================================
// Registry API — the reserve / commit / abort two-phase post.
// =============================================================================

int srv_reserve(const char *name, u8 name_len, struct Proc *poster,
                struct SrvService **svc_out, enum srv_state *prior_out) {
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return -1;
    if (!svc_out || !prior_out)                            return -1;

    // proc_stripes fail-closes to 0 for a NULL / corrupted poster; a Proc
    // the kernel cannot identify cannot post a service (and 0 would alias
    // the fail-closed sentinel srv_proc_exit_notify matches against).
    u64 stripes = proc_stripes(poster);
    if (stripes == 0) return -1;

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);

    struct SrvService *e = srv_find_locked(name, name_len);
    if (e) {
        // The name exists. A LIVE or RESERVING entry must not be displaced
        // — no stealing a running or in-flight server. Only a TOMBSTONED
        // name (prior poster exited) is re-postable.
        if (e->state != SRV_STATE_TOMBSTONED) {
            spin_unlock_irqrestore(&g_srv_registry.lock, s);
            return -1;
        }
        *prior_out = SRV_STATE_TOMBSTONED;
    } else {
        // Fresh post — claim a FREE slot.
        for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
            if (g_srv_registry.entries[i].state == SRV_STATE_FREE) {
                e = &g_srv_registry.entries[i];
                break;
            }
        }
        if (!e) {
            spin_unlock_irqrestore(&g_srv_registry.lock, s);
            return -1;   // registry full
        }
        *prior_out = SRV_STATE_FREE;
    }

    e->magic          = SRV_SERVICE_MAGIC;
    e->state          = SRV_STATE_RESERVING;
    e->name_len       = name_len;
    for (u8 i = 0; i < name_len; i++) e->name[i] = name[i];
    e->poster_stripes = stripes;
    e->poster_pid     = poster->pid;
    *svc_out = e;

    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return 0;
}

void srv_commit(struct SrvService *svc) {
    if (!svc)                          extinction("srv_commit(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_commit: bad service magic (wild pointer / corruption)");

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_commit: entry not RESERVING (double commit / lifecycle bug)");
    svc->state = SRV_STATE_LIVE;
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

void srv_abort(struct SrvService *svc, enum srv_state prior) {
    if (!svc)                          extinction("srv_abort(NULL)");
    if (svc->magic != SRV_SERVICE_MAGIC)
        extinction("srv_abort: bad service magic (wild pointer / corruption)");
    if (prior != SRV_STATE_FREE && prior != SRV_STATE_TOMBSTONED)
        extinction("srv_abort: prior state must be FREE or TOMBSTONED");

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    if (svc->state != SRV_STATE_RESERVING)
        extinction("srv_abort: entry not RESERVING (lifecycle bug)");
    if (prior == SRV_STATE_FREE) {
        // A fresh post that failed leaves no trace.
        srv_clear_locked(svc);
    } else {
        // A rebind that failed — restore the tombstone. The name stays
        // reserved; a tombstone carries no live poster identity.
        svc->state          = SRV_STATE_TOMBSTONED;
        svc->poster_stripes = 0;
        svc->poster_pid     = 0;
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

struct SrvService *srv_lookup(const char *name, u8 name_len) {
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return NULL;
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    struct SrvService *e = srv_find_locked(name, name_len);
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return e;
}

void srv_proc_exit_notify(struct Proc *p) {
    // proc_stripes fail-closes to 0; a LIVE poster always carries a
    // non-zero stripes tag, so a 0 here matches nothing.
    u64 stripes = proc_stripes(p);
    if (stripes == 0) return;

    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        struct SrvService *e = &g_srv_registry.entries[i];
        if (e->state == SRV_STATE_LIVE && e->poster_stripes == stripes) {
            // Tombstone: the name stays reserved (re-postable only by a
            // joey-marked Proc — CORVUS-DESIGN.md §6.1), the dead poster
            // identity is cleared so a tombstone carries no live poster.
            e->state          = SRV_STATE_TOMBSTONED;
            e->poster_stripes = 0;
            e->poster_pid     = 0;
        }
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

int srv_registry_count(void) {
    int n = 0;
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        if (g_srv_registry.entries[i].state != SRV_STATE_FREE) n++;
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
    return n;
}

// srv_registry_reset — TEST SUPPORT ONLY. Wipe the registry to all-FREE.
// Deliberately NOT declared in devsrv.h: no production caller exists; the
// in-kernel test harness extern-declares it so each devsrv test starts
// from an empty registry.
void srv_registry_reset(void) {
    irq_state_t s = spin_lock_irqsave(&g_srv_registry.lock);
    for (u32 i = 0; i < SRV_MAX_SERVICES; i++) {
        srv_clear_locked(&g_srv_registry.entries[i]);
    }
    spin_unlock_irqrestore(&g_srv_registry.lock, s);
}

// =============================================================================
// The devsrv Dev.
// =============================================================================
//
// At P5-corvus-srv-impl-a2 only `attach` is meaningful — it yields the
// /srv root directory Spoor. The other 15 vtable slots are graceful-fail
// stubs: the kernel requires every registered Dev to fill all 16 slots
// (ARCH §9.2; a NULL slot extincts on first dispatch — test_dev.c
// dev.vtable_slot_coverage). The walk + per-connection open path lands
// at P5-corvus-srv-impl-a3, replacing the devsrv_walk / devsrv_open /
// devsrv_close stubs.

static void devsrv_reset(void)    { /* no-op */ }
static void devsrv_init(void)     { /* no-op — the registry is static */ }
static void devsrv_shutdown(void) { /* no-op */ }

// attach — produce the /srv root directory Spoor. `spec` is unused: /srv
// is a single root, not a spec-selected namespace.
static struct Spoor *devsrv_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devsrv, QTDIR);
}

// walk — P5-corvus-srv-impl-a3 implements the real walk: a walk of
// /srv/<name> mints a per-connection KObj_Srv Spoor (specs/handles.tla
// WalkDerive). Until then /srv has no walkable children.
static struct Walkqid *devsrv_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devsrv_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// open — P5-corvus-srv-impl-a3 wires the per-connection open path (an
// open of /srv/<name> mints the kernel↔server transport). No open at a2.
static struct Spoor *devsrv_open(struct Spoor *c, int omode) {
    (void)c; (void)omode;
    return NULL;
}

static void devsrv_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
    // no-op — /srv entries are posted via SYS_POST_SERVICE, not 9P create.
}

static void devsrv_close(struct Spoor *c) {
    (void)c;
    // no-op — the /srv root Spoor holds no per-Spoor resources. a3's
    // per-connection Spoors extend this to tear down their transport.
}

static long devsrv_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static struct Block *devsrv_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devsrv_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
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

    .remove   = devsrv_remove,
    .wstat    = devsrv_wstat,
    .power    = devsrv_power,
};
