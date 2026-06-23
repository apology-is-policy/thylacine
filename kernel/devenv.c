// /env -- the per-Proc environment directory (Go Stage 4a, G15).
//
// Per ARCHITECTURE.md section 9.7. The Plan 9 Egrp / devenv idiom: a Proc's
// environment is a directory of files -- read /env/NAME for the value, write or
// create /env/NAME to set it, readdir /env for the names. Thylacine's native ABI
// passes no Unix envp at spawn, so the environment is a namespace object, not a
// syscall argument; a spawned child inherits a COPY via env_clone_into (the
// Plan 9 default-copy-on-rfork). Go's runtime reads it at startup
// (runtime/env_thylacine.go::goenvs) to populate os.Environ.
//
// dc='E'. The data + lock + monotonic-id discipline live in env.c (struct Env);
// this file is the thin Dev vtable over the env_* primitives. The mount is
// global (joey_mount_static_dev) but every op resolves the CALLING Proc's own
// env (current_thread()->proc->env), so a Proc sees only its own environment
// (I-1) -- like /proc/self. perm_enforced is false (visibility, not authority:
// a Proc's env is its own; there is nothing to leak across the per-Proc gate).
//
// A walked /env/NAME Spoor carries qid.path = the entry's MONOTONIC id; every op
// re-resolves id->entry under env->lock, so a var removed between walk and read
// fails clean rather than resolving to a different var (the net-3d slot-reuse
// discipline). No per-Spoor private state -- the value lives in the Proc's Env,
// keyed by id -- so close is the trivial dev_simple_close (no UAF surface).

#include <thylacine/dev.h>
#include <thylacine/env.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// qid.path == 0 is the /env root (QTDIR); any other path is an entry id (QTFILE,
// QTFILE == 0x00). Entry ids start at 1 (env.c), so they never collide with 0.
#define ENV_QID_ROOT  0ULL

// The calling Proc whose environment this op acts on. NULL only off a thread
// (never in a real syscall path); callers fail closed on NULL.
static struct Proc *env_proc(void) {
    struct Thread *t = current_thread();
    return t ? t->proc : NULL;
}

static u32 cstr_len(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') n++;       // the handler NUL-terminates within NAME_MAX
    return n;
}

// =============================================================================
// Walk.
// =============================================================================

// Single-step walk dispatch. cur_path is the current Spoor's qid.path; resolves
// `name` against the calling Proc's env. Fills *out_qid; true on hit.
static bool walk_one(struct Proc *p, u64 cur_path, const char *name,
                     struct Qid *out_qid) {
    out_qid->path = 0;
    out_qid->vers = 0;
    out_qid->type = 0;
    out_qid->pad[0] = out_qid->pad[1] = out_qid->pad[2] = 0;

    // ".." -- /env is single-level; every entry's parent is the root.
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out_qid->path = ENV_QID_ROOT;
        out_qid->type = QTDIR;
        return true;
    }

    if (cur_path == ENV_QID_ROOT) {
        u64 id = env_lookup(p, name, cstr_len(name));
        if (id == 0) return false;                 // no such var (or no env)
        out_qid->path = id;
        out_qid->type = QTFILE;
        return true;
    }
    // From a value entry: a file has no children.
    return false;
}

// =============================================================================
// Readdir wire format (the 9P2000.L dirent the SYS_READDIR handler parses):
// qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name. `cookie` is the
// resume cursor -- STRICTLY INCREASING and never 0 (0 means end-of-directory).
// Returns the entry size, or 0 if it does not fit (whole entries only). Mirrors
// devpci/devhw.
// =============================================================================

static long emit_dirent(u8 *out, long cap, long pos, u64 qpath, u8 qtype,
                        u64 cookie, const char *name, u32 nlen) {
    long entry = 24 + (long)nlen;
    if (pos + entry > cap) return 0;
    out[pos + 0] = qtype;
    out[pos + 1] = 0; out[pos + 2] = 0; out[pos + 3] = 0; out[pos + 4] = 0;
    for (int b = 0; b < 8; b++) out[pos + 5 + b]  = (u8)(qpath >> (8 * b));
    for (int b = 0; b < 8; b++) out[pos + 13 + b] = (u8)(cookie >> (8 * b));
    out[pos + 21] = qtype;
    out[pos + 22] = (u8)(nlen & 0xffu);
    out[pos + 23] = (u8)((nlen >> 8) & 0xffu);
    for (u32 i = 0; i < nlen; i++) out[pos + 24 + (long)i] = (u8)name[i];
    return entry;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devenv_reset(void)    { /* no-op */ }
static void devenv_init(void)     { /* no-op */ }
static void devenv_shutdown(void) { /* no-op */ }

static struct Spoor *devenv_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devenv, QTDIR);
}

static struct Walkqid *devenv_walk(struct Spoor *c, struct Spoor *nc,
                                   const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;
    struct Proc *p = env_proc();

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    // Reuse-nc contract (#57a): a non-NULL nc is the caller's pre-clone and MUST
    // be the returned wq->spoor (a 0-element walk returns it unchanged with
    // nqid == 0, the shape clone_walk_zero needs to cross the /env mount). nc ==
    // NULL is the legacy direct-call shape (kernel tests).
    struct Spoor *cur;
    if (nc) {
        cur = nc;
        cur->qid = c->qid;
    } else {
        cur = spoor_clone(c);
        if (!cur) { walkqid_free(wq); return NULL; }
    }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!walk_one(p, cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devenv_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devenv_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    // OTRUNC (0x10) on a value file resets it -- os.WriteFile / os.Create open
    // O_TRUNC then write the whole value. The root is a directory (no truncate).
    if (c->qid.path != ENV_QID_ROOT && (omode & 0x10)) {
        struct Proc *p = env_proc();
        if (p) env_truncate(p, c->qid.path);
    }
    return dev_simple_open(c, omode);
}

static struct Spoor *devenv_create(struct Spoor *c, const char *name, int omode,
                                   u32 perm, u32 gid) {
    (void)gid;
    if (!c || !name) return NULL;
    if (perm & SYS_WALK_CREATE_DMDIR) return NULL;     // /env has no sub-directories
    struct Proc *p = env_proc();
    if (!p) return NULL;

    u64 id = env_create(p, name, cstr_len(name));
    if (id == 0) return NULL;                          // bad name / OOM / bounds
    if (omode & 0x10) env_truncate(p, id);             // OTRUNC on a re-created name

    // Transition the parent-dir Spoor `c` into the new file (Plan 9 create
    // semantics; mirrors dev9p_create): qid -> the entry id, opened.
    c->qid.path = id;
    c->qid.vers = 0;
    c->qid.type = QTFILE;
    c->flag    |= COPEN;
    c->offset   = 0;
    c->mode     = omode;
    return c;
}

static void devenv_close(struct Spoor *c) {
    dev_simple_close(c);
}

static long devenv_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (c->qid.path == ENV_QID_ROOT) return -1;        // a dir enumerates via readdir
    struct Proc *p = env_proc();
    if (!p) return -1;
    return env_read(p, c->qid.path, off, buf, n);
}

static struct Block *devenv_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devenv_write(struct Spoor *c, const void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (c->qid.path == ENV_QID_ROOT) return -1;        // cannot write the directory
    struct Proc *p = env_proc();
    if (!p) return -1;
    return env_write(p, c->qid.path, off, buf, n);
}

static long devenv_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Enumerate the env. The root lists one file per live entry; cookie == the
// entry's monotonic id (strictly increasing, never 0). `off` is the resume
// cursor (the last id emitted). A value file does not enumerate.
static long devenv_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n <= 0) return 0;
    if (off < 0) return -1;
    if (c->qid.path != ENV_QID_ROOT) return -1;
    struct Proc *p = env_proc();
    if (!p) return 0;                                  // no env -> empty directory

    u8 *out = (u8 *)buf;
    long pos = 0;
    u64 cursor = (u64)off;
    for (;;) {
        u64 id = 0;
        char nm[ENV_NAME_MAX];
        u32 nlen = 0;
        if (!env_iter(p, cursor, &id, nm, (u32)sizeof(nm), &nlen)) break;
        long e = emit_dirent(out, n, pos, id, QTFILE, id, nm, nlen);
        if (e == 0) {
            if (pos == 0) return -1;                   // first entry too big for buf
            break;                                     // resume next call from cursor
        }
        pos   += e;
        cursor = id;                                   // advance past this entry
    }
    return pos;
}

// Unset: os.Remove("/env/NAME") -> SYS_UNLINK -> here. /env has no directories,
// so SYS_UNLINK_REMOVEDIR is rejected.
static int devenv_unlink(struct Spoor *parent, const char *name, u32 flags) {
    (void)parent;
    if (!name) return -1;
    if (flags & SYS_UNLINK_REMOVEDIR) return -1;
    struct Proc *p = env_proc();
    if (!p) return -1;
    return env_unset(p, name, cstr_len(name)) ? 0 : -1;
}

static void devenv_remove(struct Spoor *c) {
    (void)c;
}

static int devenv_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devenv_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devenv = {
    .dc       = 'E',
    .name     = "env",

    .reset    = devenv_reset,
    .init     = devenv_init,
    .shutdown = devenv_shutdown,

    .attach   = devenv_attach,
    .walk     = devenv_walk,
    .stat     = devenv_stat,

    .open     = devenv_open,
    .create   = devenv_create,
    .close    = devenv_close,

    .read     = devenv_read,
    .bread    = devenv_bread,
    .write    = devenv_write,
    .bwrite   = devenv_bwrite,
    .readdir  = devenv_readdir,
    .unlink   = devenv_unlink,

    .remove   = devenv_remove,
    .wstat    = devenv_wstat,
    .power    = devenv_power,
};
