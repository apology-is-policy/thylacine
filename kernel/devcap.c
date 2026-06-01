// devcap — the hostowner-elevation `cap` device.
//
// Per CORVUS-DESIGN.md §5.5 + §5.5.1. The kernel side of corvus's two-
// phase, file-mediated grant of CAP_HOSTOWNER (or any future elevation-
// only capability). Two write-only files under /cap:
//
//   /grant — corvus writes {cap_mask, target_stripes} (CAP_GRANT_WRITE_LEN
//            bytes); kernel records a pending grant. Gated on the writer
//            holding CAP_GRANT_HOSTOWNER.
//   /use   — the target Proc writes {cap_mask} (CAP_USE_WRITE_LEN bytes);
//            kernel ORs cap_mask into writer's caps, consumes the grant.
//            Gated on the writer holding PROC_FLAG_CONSOLE_ATTACHED AND
//            a non-expired pending grant existing for the writer's stripes
//            with a matching cap_mask.
//
// Defense in depth: corvus (userspace) verifies the system passphrase, a
// check the kernel has no notion of. The kernel verifies console
// attachment at redemption — the load-bearing gate that holds even if
// corvus is buggy or compromised. A compromised corvus can register
// grants for arbitrary stripes, but only a console-attached writer may
// redeem; corvus elevating a network process is structurally impossible.
//
// Spec: specs/corvus.tla — HostownerGrant + HostownerRequiresConsole.

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/devcap.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"
#include "../mm/slub.h"

// =============================================================================
// Pending-grant table.
// =============================================================================

enum cap_grant_state {
    CAP_GRANT_FREE    = 0,    // slot unused (BSS zero-init reads as FREE)
    CAP_GRANT_PENDING = 1,    // grant registered, awaiting redemption
};

// A-4a: a pending grant is one of two kinds. The kind is set at register and
// determines the redeem gate + effect (see cap_redeem_grant_for_writer). It
// rides on the entry (NOT on which /grant write length produced it after the
// fact), so a SINGLE locked lookup at redeem reads the kind atomically -- no
// peek/redeem TOCTOU.
enum cap_grant_kind {
    CAP_GRANT_KIND_HOSTOWNER = 0,  // legacy console-gated CAP_HOSTOWNER grant
    CAP_GRANT_KIND_CLEARANCE = 1,  // A-4a clearance grant -> creates a legate
};

struct cap_grant_entry {
    enum cap_grant_state state;
    enum cap_grant_kind  kind;        // discriminates redeem semantics
    caps_t               cap_mask;     // capabilities to confer at redeem
    u64                  target_stripes;  // writer's stripes must match
    u64                  expiry_ns;    // REDEMPTION-window deadline (timer_now_ns;
                                       // <= it == the grant itself expired)
    u64                  valid_for_ns; // CLEARANCE: legate lifetime DURATION
                                       // (0 = none). legate_valid_until is
                                       // computed = now + this AT redeem, so the
                                       // window opens when the caps actually land
                                       // (avoids any userspace/kernel clock skew).
    u32                  session_id;   // CLEARANCE: corvus audit tag (-> legate)
};

struct cap_grant_table {
    spin_lock_t            lock;
    struct cap_grant_entry entries[CAP_GRANT_MAX];
};

static struct cap_grant_table g_cap_grants;

// =============================================================================
// Spoor aux (the leaf-file kind tag).
// =============================================================================

// Each walked leaf Spoor carries a small kmalloc'd aux holding only a
// magic that identifies which leaf this is. Two magics, no payload — the
// magic alone discriminates /grant vs /use at write time.
#define DEVCAP_GRANT_MAGIC  0x444341505F47524EULL   // 'DCAPGRN' or similar
#define DEVCAP_USE_MAGIC    0x444341505F555345ULL   // 'DCAP_USE' or similar

struct devcap_leaf_ref {
    u64 magic;
};

// =============================================================================
// Internals.
// =============================================================================

// Expire+find-free in one pass. PRECONDITION: caller holds the table
// lock. Returns the index of a slot the caller may write, or -1 if all
// slots hold non-expired entries.
//
// "Expire" means: any entry whose expiry_ns is at or before `now_ns` is
// reset to FREE; the slot becomes available without a separate sweep.
static void cap_clear_locked(struct cap_grant_entry *e) {
    e->state          = CAP_GRANT_FREE;
    e->kind           = CAP_GRANT_KIND_HOSTOWNER;
    e->cap_mask       = 0;
    e->target_stripes = 0;
    e->expiry_ns      = 0;
    e->valid_for_ns   = 0;
    e->session_id     = 0;
}

static int cap_find_free_locked(u64 now_ns) {
    int free_idx = -1;
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) {
        struct cap_grant_entry *e = &g_cap_grants.entries[i];
        if (e->state == CAP_GRANT_PENDING && e->expiry_ns <= now_ns)
            cap_clear_locked(e);
        if (e->state == CAP_GRANT_FREE && free_idx < 0) {
            free_idx = (int)i;
        }
    }
    return free_idx;
}

// Find a PENDING entry whose target_stripes matches `stripes`. Returns
// the index or -1. PRECONDITION: caller holds the table lock. Does NOT
// expire — caller checks expiry against the returned entry's expiry_ns.
static int cap_find_stripes_locked(u64 stripes) {
    if (stripes == 0) return -1;   // 0 is the fail-closed sentinel
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) {
        struct cap_grant_entry *e = &g_cap_grants.entries[i];
        if (e->state == CAP_GRANT_PENDING && e->target_stripes == stripes)
            return (int)i;
    }
    return -1;
}

// Write all fields of a slot for a fresh/replacing PENDING grant. Routing
// BOTH register paths through this is load-bearing: a re-register over an
// existing slot of the OTHER kind must reset EVERY discriminator field
// (kind, valid_for_ns, session_id), or a stale clearance grant's fields
// could survive under a hostowner cap_mask (or vice versa) and a redeem
// would apply the wrong kind's semantics. PRECONDITION: caller holds the
// table lock.
static void cap_set_entry_locked(struct cap_grant_entry *e,
                                 enum cap_grant_kind kind, caps_t cap_mask,
                                 u64 target_stripes, u64 expiry_ns,
                                 u64 valid_for_ns, u32 session_id) {
    e->state          = CAP_GRANT_PENDING;
    e->kind           = kind;
    e->cap_mask       = cap_mask;
    e->target_stripes = target_stripes;
    e->expiry_ns      = expiry_ns;
    e->valid_for_ns   = valid_for_ns;
    e->session_id     = session_id;
}

// Find the slot index to write for `target_stripes` -- the existing PENDING
// entry for those stripes (re-register replaces in place) or a free slot.
// Returns -1 if no slot is available. PRECONDITION: caller holds the lock.
static int cap_slot_for_stripes_locked(u64 target_stripes, u64 now) {
    int existing = cap_find_stripes_locked(target_stripes);
    if (existing >= 0) return existing;
    return cap_find_free_locked(now);
}

// =============================================================================
// Grant + redeem cores (exposed for tests + the Dev write op).
// =============================================================================

long cap_register_grant_for_writer(struct Proc *writer,
                                   caps_t cap_mask, u64 target_stripes) {
    if (!writer)                                      return -1;
    if (target_stripes == 0)                          return -1;
    if (cap_mask == 0)                                return -1;
    if ((cap_mask & ~(caps_t)CAP_GRANTABLE) != 0)    return -1;
    // Writer gate: must hold CAP_GRANT_HOSTOWNER. Single point — the
    // CAP_GRANT_HOSTOWNER bit is the only authority to register a grant.
    if ((writer->caps & (caps_t)CAP_GRANT_HOSTOWNER) == 0) return -1;

    u64 now = timer_now_ns();
    u64 expiry = now + CAP_GRANT_EXPIRY_NS;

    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);

    // Re-register semantics (CORVUS-DESIGN §5.5.1 "at most one pending
    // grant per tag; a re-register replaces it"): if a PENDING entry
    // already targets these stripes, overwrite it in place.
    int idx = cap_slot_for_stripes_locked(target_stripes, now);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;        // table full of non-expired entries
    }

    // HOSTOWNER kind; valid_for_ns + session_id are 0 (clearance-only) -- set
    // explicitly so a re-register over a stale clearance slot resets them.
    cap_set_entry_locked(&g_cap_grants.entries[idx], CAP_GRANT_KIND_HOSTOWNER,
                         cap_mask, target_stripes, expiry, 0, 0);

    spin_unlock_irqrestore(&g_cap_grants.lock, s);
    return (long)CAP_GRANT_WRITE_LEN;
}

// A-4a clearance /grant core (32-byte form). See devcap.h. Gated on
// CAP_GRANT_CLEARANCE; cap_mask must be a non-empty subset of
// CAP_GRANTABLE_CLEARANCE; session_id must be nonzero (sentinel) and fit u32.
long cap_register_clearance_grant_for_writer(struct Proc *writer,
                                             caps_t cap_mask, u64 target_stripes,
                                             u64 valid_for_ns, u64 session_id) {
    if (!writer)                                              return -1;
    if (target_stripes == 0)                                 return -1;
    if (cap_mask == 0)                                       return -1;
    if ((cap_mask & ~(caps_t)CAP_GRANTABLE_CLEARANCE) != 0)  return -1;
    if (session_id == 0 || session_id > 0xFFFFFFFFull)       return -1;
    // Writer gate: must hold CAP_GRANT_CLEARANCE (corvus). The clearance
    // analog of the hostowner grant's CAP_GRANT_HOSTOWNER gate.
    if ((writer->caps & (caps_t)CAP_GRANT_CLEARANCE) == 0)   return -1;

    u64 now = timer_now_ns();
    u64 expiry = now + CAP_GRANT_EXPIRY_NS;

    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);

    int idx = cap_slot_for_stripes_locked(target_stripes, now);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;        // table full of non-expired entries
    }

    cap_set_entry_locked(&g_cap_grants.entries[idx], CAP_GRANT_KIND_CLEARANCE,
                         cap_mask, target_stripes, expiry, valid_for_ns,
                         (u32)session_id);

    spin_unlock_irqrestore(&g_cap_grants.lock, s);
    return (long)CAP_GRANT_CLEARANCE_WRITE_LEN;
}

long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask) {
    if (!writer)        return -1;
    if (cap_mask == 0)  return -1;    // a redeem must request >= one cap

    u64 stripes = proc_stripes(writer);
    if (stripes == 0)   return -1;

    u64 now = timer_now_ns();

    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);

    // ONE locked lookup: the grant's `kind` is read atomically with the rest,
    // so there is no peek-then-redeem TOCTOU (a concurrent re-register that
    // flips the kind for these stripes either lands fully before this lookup
    // or fully after the consume).
    int idx = cap_find_stripes_locked(stripes);
    if (idx < 0) {
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;
    }

    struct cap_grant_entry *e = &g_cap_grants.entries[idx];

    // Expired? Treat as not-found; clear the slot.
    if (e->expiry_ns <= now) {
        cap_clear_locked(e);
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;
    }

    if (e->kind == CAP_GRANT_KIND_CLEARANCE) {
        // A-4a clearance redeem -> creates a legate. NO console gate (the
        // high-stakes auth was enforced corvus-side via auth_required BEFORE
        // the grant was ever registered; CAP_GRANT_CLEARANCE is corvus-only).
        // self_restriction (I-2): the request must be a non-empty SUBSET of the
        // granted set -- the Proc voluntarily narrows below the ceiling. A
        // request reaching BEYOND the grant is a bug/attack -> fail closed
        // WITHOUT consuming (the legitimate holder may still redeem).
        if ((cap_mask & ~e->cap_mask) != 0) {
            spin_unlock_irqrestore(&g_cap_grants.lock, s);
            return -1;
        }
        caps_t to_or     = cap_mask;          // == cap_mask & e->cap_mask (subset)
        u64    valid_for = e->valid_for_ns;
        u32    session   = e->session_id;
        cap_clear_locked(e);                  // one-shot consume
        spin_unlock_irqrestore(&g_cap_grants.lock, s);

        // The legate window opens NOW -- when the caps actually land -- not at
        // grant-register time, so a slow redeem doesn't shorten the window and
        // there is no userspace/kernel clock-domain dependency. Saturating add
        // (A-4a audit F3): a valid_for so large that now + valid_for would wrap
        // is clamped to ~0 ("never expires by time"; still scope-bounded by the
        // root's exit) rather than wrapping to a small deadline -- or, in the
        // pathological wrap-to-exactly-0 case, silently aliasing the 0 sentinel
        // that means "no time bound." Fail-safe either way; the clamp removes the
        // alias so a nonzero request never degrades into an unbounded window.
        u64 valid_until = (valid_for == 0)                ? 0
                        : (valid_for > (~0ull - now))     ? ~0ull
                        : now + valid_for;
        proc_become_legate(writer, to_or, session, valid_until);
        return (long)CAP_USE_WRITE_LEN;
    }

    // HOSTOWNER redeem (unchanged v1.0 semantics): console-gated + the request
    // must EQUAL the granted cap. Gate console AFTER the lookup so the kind is
    // known -- a clearance grant for these stripes was already handled above,
    // so this arm only ever sees a hostowner grant. Equality (not subset) keeps
    // the protocol explicit (a /use asking for a different cap is a bug/attack);
    // the grant is NOT consumed on a gate failure (the legitimate console
    // holder may still redeem).
    if (!proc_is_console_attached(writer)) {
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;
    }
    if (e->cap_mask != cap_mask) {
        spin_unlock_irqrestore(&g_cap_grants.lock, s);
        return -1;
    }

    caps_t to_or = e->cap_mask;
    cap_clear_locked(e);

    spin_unlock_irqrestore(&g_cap_grants.lock, s);

    // The capability lands on the WRITER's Proc. v1.0 hostowner redeemers
    // (corvus / login) make the non-atomic OR benign in practice; the
    // clearance path above uses proc_become_legate's __atomic_fetch_or.
    writer->caps |= to_or;
    return (long)CAP_USE_WRITE_LEN;
}

// =============================================================================
// Bookkeeping.
// =============================================================================

int cap_pending_count(void) {
    int n = 0;
    u64 now = timer_now_ns();
    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) {
        struct cap_grant_entry *e = &g_cap_grants.entries[i];
        if (e->state == CAP_GRANT_PENDING && e->expiry_ns > now) n++;
    }
    spin_unlock_irqrestore(&g_cap_grants.lock, s);
    return n;
}

void cap_proc_exit_notify(struct Proc *p) {
    if (!p) return;
    u64 stripes = proc_stripes(p);
    if (stripes == 0) return;
    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) {
        struct cap_grant_entry *e = &g_cap_grants.entries[i];
        if (e->state == CAP_GRANT_PENDING && e->target_stripes == stripes)
            cap_clear_locked(e);
    }
    spin_unlock_irqrestore(&g_cap_grants.lock, s);
}

void cap_reset_table(void) {
    irq_state_t s = spin_lock_irqsave(&g_cap_grants.lock);
    for (u32 i = 0; i < CAP_GRANT_MAX; i++) cap_clear_locked(&g_cap_grants.entries[i]);
    spin_unlock_irqrestore(&g_cap_grants.lock, s);
}

// =============================================================================
// Dev vtable.
// =============================================================================

static void devcap_reset(void)    { /* no-op */ }
static void devcap_init(void)     { /* no-op — BSS zeros the table */ }
static void devcap_shutdown(void) { /* no-op */ }

// attach — produce the /cap root directory Spoor. `spec` is unused.
static struct Spoor *devcap_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devcap, QTDIR);
}

// Length-bounded name equality against a literal C string.
static bool devcap_name_eq(const char *s, u32 slen, const char *lit) {
    for (u32 i = 0; i < slen; i++) {
        if (lit[i] == '\0') return false;
        if (s[i] != lit[i]) return false;
    }
    return lit[slen] == '\0';
}

// walk — the /cap root walks two single-component names: "grant" and
// "use". Each yields a leaf Spoor (QTFILE) whose aux carries the magic
// identifying which leaf.
static struct Walkqid *devcap_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c || c->dc != 'k' || c->aux != NULL) return NULL;   // root only
    if (!nc || nname < 0)                     return NULL;

    if (nname == 0) {
        // Clone — nc is the caller's shallow copy of the root (aux NULL).
        struct Walkqid *w = walkqid_alloc(1);
        if (!w) return NULL;
        w->nqid  = 0;
        w->spoor = nc;
        return w;
    }
    if (nname != 1) return NULL;

    const char *s = name[0];
    if (!s) return NULL;
    u32 len = 0;
    while (len < 16 && s[len] != '\0') len++;
    if (len == 0 || s[len] != '\0') return NULL;

    u64 leaf_magic;
    if      (devcap_name_eq(s, len, "grant")) leaf_magic = DEVCAP_GRANT_MAGIC;
    else if (devcap_name_eq(s, len, "use"))   leaf_magic = DEVCAP_USE_MAGIC;
    else                                       return NULL;

    struct devcap_leaf_ref *ref = kmalloc(sizeof(*ref), KP_ZERO);
    if (!ref) return NULL;
    ref->magic = leaf_magic;

    struct Walkqid *w = walkqid_alloc(1);
    if (!w) { kfree(ref); return NULL; }

    nc->aux      = ref;
    nc->qid.type = QTFILE;
    nc->qid.path = leaf_magic;
    nc->qid.vers = 0;
    w->nqid    = 1;
    w->qid[0]  = nc->qid;
    w->spoor   = nc;
    return w;
}

static int devcap_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// open — leaf files accept OWRITE only (and OWRITE | OTRUNC variants); a
// read attempt fails-closed (these are write-only by CORVUS-DESIGN §5.5.1).
// The root Spoor isn't opened directly.
static struct Spoor *devcap_open(struct Spoor *c, int omode) {
    if (!c || c->dc != 'k' || !c->aux) return NULL;
    u64 m = *(const u64 *)c->aux;
    if (m != DEVCAP_GRANT_MAGIC && m != DEVCAP_USE_MAGIC) return NULL;
    // Accept any mode that includes write; refuse read-only or exec.
    // OWRITE / ORDWR are both fine — we just need a write path.
    int access = omode & 3;     // OREAD=0, OWRITE=1, ORDWR=2, OEXEC=3
    if (access == 0 || access == 3) return NULL;   // no OREAD, no OEXEC
    c->flag |= COPEN;
    return c;
}

static struct Spoor *devcap_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

// close — release the kmalloc'd leaf-ref aux. The root has no aux.
static void devcap_close(struct Spoor *c) {
    if (!c || c->dc != 'k' || !c->aux) return;
    u64 m = *(const u64 *)c->aux;
    if (m != DEVCAP_GRANT_MAGIC && m != DEVCAP_USE_MAGIC)
        extinction("devcap_close: Spoor aux has unknown magic (corruption)");
    struct devcap_leaf_ref *ref = (struct devcap_leaf_ref *)c->aux;
    ref->magic = 0;
    kfree(ref);
    c->aux = NULL;
}

// read — write-only files; reading is a hard error.
static long devcap_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static struct Block *devcap_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Decode a u64 LE from a byte buffer. Caller has bounds-checked.
static u64 le64(const u8 *p) {
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24)
         | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

// write — dispatch to grant or use core based on the leaf magic.
// Message-oriented: each write is exactly one fixed-size frame.
// Returns the frame length on success, -1 on any fail-closed condition.
static long devcap_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    if (!c || c->dc != 'k' || !c->aux) return -1;
    if (n < 0 || !buf)                  return -1;
    u64 m = *(const u64 *)c->aux;

    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;
    struct Proc *writer = t->proc;

    const u8 *b = (const u8 *)buf;

    if (m == DEVCAP_GRANT_MAGIC) {
        // Length-discriminated: 16 bytes = hostowner grant; 32 = A-4a clearance
        // grant. The two never collide. Any other length fails closed.
        if (n == (long)CAP_GRANT_WRITE_LEN) {
            caps_t cap_mask        = (caps_t)le64(b);
            u64    target_stripes  = le64(b + 8);
            return cap_register_grant_for_writer(writer, cap_mask, target_stripes);
        }
        if (n == (long)CAP_GRANT_CLEARANCE_WRITE_LEN) {
            caps_t cap_mask        = (caps_t)le64(b);
            u64    target_stripes  = le64(b + 8);
            u64    valid_for_ns    = le64(b + 16);
            u64    session_id      = le64(b + 24);
            return cap_register_clearance_grant_for_writer(writer, cap_mask,
                       target_stripes, valid_for_ns, session_id);
        }
        return -1;
    }
    if (m == DEVCAP_USE_MAGIC) {
        if (n != (long)CAP_USE_WRITE_LEN) return -1;
        caps_t cap_mask = (caps_t)le64(b);
        return cap_redeem_grant_for_writer(writer, cap_mask);
    }
    extinction("devcap_write: Spoor aux has unknown magic (corruption)");
    return -1;   // unreachable
}

static long devcap_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devcap_remove(struct Spoor *c) { (void)c; }
static int  devcap_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}
static struct Spoor *devcap_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devcap = {
    .dc       = 'k',          // 'k' for "kapability"; 'C' is taken by devctl
    .name     = "cap",

    .reset    = devcap_reset,
    .init     = devcap_init,
    .shutdown = devcap_shutdown,

    .attach   = devcap_attach,
    .walk     = devcap_walk,
    .stat     = devcap_stat,

    .open     = devcap_open,
    .create   = devcap_create,
    .close    = devcap_close,

    .read     = devcap_read,
    .bread    = devcap_bread,
    .write    = devcap_write,
    .bwrite   = devcap_bwrite,

    .poll     = NULL,        // not pollable at v1.0 (write-only message channels)

    .remove   = devcap_remove,
    .wstat    = devcap_wstat,
    .power    = devcap_power,
};
