// /dev — the kernel char-device directory (#57b).
//
// Per ARCHITECTURE.md §9.4. Plan 9 idiom: a single aggregating directory Dev
// (the `#c`-as-a-directory model) serving the kernel char devices as named
// leaves. Thylacine's established one-Dev-many-leaves pattern (devctl / devproc
// / devcap): one walk, one qid-dispatched read/write, one open site.
//
//   /dev/null     — bit bucket (reads EOF; writes consumed)
//   /dev/zero     — zero-fill reads; consume writes
//   /dev/full     — zero-fill reads; writes fail (full disk)
//   /dev/random   — CSPRNG (kern_random_bytes); writes consumed
//   /dev/urandom  — alias of random (POSIX compat; same handler)
//   /dev/cons     — the console (delegates to the shared cons.c API)
//   /dev/consctl  — console mode control (v1.0-modeless; termios is LS-8 #952)
//
// dc='d'. The trivial leaves (null/zero/full/random/urandom) are world-rw and
// UNGATED -- the same on every Unix. cons/consctl are the console: `devdev_open`
// enforces the I-27 gate-at-namespace-open -- a console-attach check IDENTICAL to
// SYS_CONSOLE_OPEN's, so binding /dev/cons as a walkable path adds NO ungated
// front door to the single-reader console (see IDENTITY-DESIGN.md §9.8).

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/random.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// =============================================================================
// Qid encoding (mirrors devctl).
// =============================================================================
//
// path = 0            => root /dev directory (QTDIR)
// path = leaf_kind    => leaf file (QTFILE), kind != 0

enum {
    DEV_KIND_ROOT    = 0,
    DEV_KIND_NULL    = 1,
    DEV_KIND_ZERO    = 2,
    DEV_KIND_FULL    = 3,
    DEV_KIND_RANDOM  = 4,
    DEV_KIND_URANDOM = 5,
    DEV_KIND_CONS    = 6,
    DEV_KIND_CONSCTL = 7,
};

#define DEV_QID_ROOT_PATH  0ULL

struct dev_leaf {
    const char *name;
    u32         kind;
};

static const struct dev_leaf g_dev_leaves[] = {
    { "null",    DEV_KIND_NULL    },
    { "zero",    DEV_KIND_ZERO    },
    { "full",    DEV_KIND_FULL    },
    { "random",  DEV_KIND_RANDOM  },
    { "urandom", DEV_KIND_URANDOM },
    { "cons",    DEV_KIND_CONS    },
    { "consctl", DEV_KIND_CONSCTL },
};

#define DEV_LEAF_COUNT  (sizeof(g_dev_leaves) / sizeof(g_dev_leaves[0]))

// cons + consctl are the console -- the I-27 gated leaves.
static bool dev_kind_is_console(u32 kind) {
    return kind == DEV_KIND_CONS || kind == DEV_KIND_CONSCTL;
}

// =============================================================================
// Walk.
// =============================================================================

static bool name_eq(const char *a, const char *b) {
    int j = 0;
    while (a[j] && b[j] && a[j] == b[j]) j++;
    return a[j] == 0 && b[j] == 0;
}

// Single-step walk dispatch. cur_path is the current Spoor's qid.path.
// Fills *out_qid + returns true on success; returns false on miss.
static bool walk_one(u64 cur_path, const char *name, struct Qid *out_qid) {
    out_qid->path = 0;
    out_qid->vers = 0;
    out_qid->type = 0;
    out_qid->pad[0] = out_qid->pad[1] = out_qid->pad[2] = 0;

    // ".." goes up. /dev is single-level -- root and any leaf both have the
    // dev's apex as their parent.
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out_qid->path = DEV_QID_ROOT_PATH;
        out_qid->type = QTDIR;
        return true;
    }

    if (cur_path == DEV_QID_ROOT_PATH) {
        for (size_t i = 0; i < DEV_LEAF_COUNT; i++) {
            if (name_eq(g_dev_leaves[i].name, name)) {
                out_qid->path = (u64)g_dev_leaves[i].kind;
                out_qid->type = QTFILE;
                return true;
            }
        }
        return false;
    }

    // From a leaf qid: walk has no meaning (leaves are files).
    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devdev_reset(void)    { /* no-op */ }
static void devdev_init(void)     { /* no-op */ }
static void devdev_shutdown(void) { /* no-op */ }

static struct Spoor *devdev_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devdev, QTDIR);
}

static struct Walkqid *devdev_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    // Reuse-nc contract (#57a lesson): a non-NULL nc is the caller's pre-clone
    // and MUST be the returned wq->spoor -- a 0-element walk then returns nc
    // unchanged with nqid == 0, the shape clone_walk_zero needs to cross the
    // /dev mount. nc == NULL is the legacy direct-call shape (kernel tests).
    // Without it a mounted devdev is unreachable through stalk (wq->spoor != nc
    // -> reject), the same bug devramfs/devctl/devproc carried before mounting.
    struct Spoor *cur;
    if (nc) {
        cur = nc;
        cur->qid = c->qid;
    } else {
        cur = spoor_clone(c);
        if (!cur) {
            walkqid_free(wq);
            return NULL;
        }
    }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!walk_one(cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devdev_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

// The I-27 gate-at-namespace-open. The cons/consctl leaves are the console --
// a single-reader resource. An ungated open("/dev/cons") would let any EL0 Proc
// become that reader and steal the getty's console input (the A-5a-F2 break the
// SYS_CONSOLE_OPEN gate closed; a login passphrase would land in the thief's
// read). So this enforces the SAME proc_is_console_attached gate the syscall
// does: walk("/dev/cons") resolves the name, but open fails NULL (-> walk-open
// -1) for a non-attached caller. Only the console-attach holder (joey
// pre-relinquish / post-SAK corvus) can open it -- exactly as via the syscall.
// The gate is at OPEN, so it covers all subsequent read AND write (a non-attached
// Proc cannot even spoof console output). The trivial leaves pass through ungated.
static struct Spoor *devdev_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    if (dev_kind_is_console((u32)c->qid.path)) {
        struct Thread *t = current_thread();
        if (!t || !proc_is_console_attached(t->proc)) return NULL;
    }
    return dev_simple_open(c, omode);
}

static struct Spoor *devdev_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devdev_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devdev_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    if (!c || !buf) return -1;
    if (n < 0) return -1;

    u32 kind = (u32)c->qid.path;
    switch (kind) {
    case DEV_KIND_ROOT:                       // readdir deferred (match devctl)
        return -1;
    case DEV_KIND_NULL:                        // EOF on every read
        return 0;
    case DEV_KIND_ZERO:
    case DEV_KIND_FULL: {                       // NUL-fill the caller's buffer
        if (n == 0) return 0;
        u8 *out = (u8 *)buf;
        for (long i = 0; i < n; i++) out[i] = 0;
        return n;
    }
    case DEV_KIND_RANDOM:
    case DEV_KIND_URANDOM:                      // CSPRNG (fail-closed -> -1)
        return kern_random_bytes(buf, n);
    case DEV_KIND_CONS:                         // the shared console-input drain
        return cons_input_read(buf, n);
    case DEV_KIND_CONSCTL:                      // no modes at v1.0 (termios = LS-8)
        return 0;
    default:
        return -1;
    }
}

static struct Block *devdev_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devdev_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    if (!c) return -1;
    if (n < 0) return -1;

    u32 kind = (u32)c->qid.path;
    switch (kind) {
    case DEV_KIND_NULL:
    case DEV_KIND_ZERO:                         // silently consume
        return n;
    case DEV_KIND_RANDOM:
    case DEV_KIND_URANDOM:                      // consume (pool stir-on-write is a
        return n;                               // v1.x ergonomic; the CSPRNG
                                                // reseeds on its own cadence)
    case DEV_KIND_FULL:                         // full disk -- writes fail
        return -1;
    case DEV_KIND_CONS:                         // the shared console-output path
        return cons_output_write(buf, n);
    case DEV_KIND_CONSCTL:                      // no modes at v1.0
        return -1;
    case DEV_KIND_ROOT:
    default:
        return -1;
    }
}

static long devdev_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devdev_remove(struct Spoor *c) {
    (void)c;
}

static int devdev_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devdev_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devdev = {
    .dc       = 'd',
    .name     = "dev",

    .reset    = devdev_reset,
    .init     = devdev_init,
    .shutdown = devdev_shutdown,

    .attach   = devdev_attach,
    .walk     = devdev_walk,
    .stat     = devdev_stat,

    .open     = devdev_open,
    .create   = devdev_create,
    .close    = devdev_close,

    .read     = devdev_read,
    .bread    = devdev_bread,
    .write    = devdev_write,
    .bwrite   = devdev_bwrite,

    .remove   = devdev_remove,
    .wstat    = devdev_wstat,
    .power    = devdev_power,
};
