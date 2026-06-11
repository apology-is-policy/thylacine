// /proc — synthetic Dev exposing process state (P4-C).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. Plan 9 idiom: walk to
// /proc/<pid>/<file> and read text-format process state. v1.0 P4-C
// lands these files:
//
//   /proc/<pid>/status   — pid, state, threads, exit_status text
//   /proc/<pid>/cmdline  — argv[0]; placeholder at v1.0 (no argv yet)
//   /proc/<pid>/ctl      — write commands: kill / killgrp (A-4b)
//   /proc/<pid>/ns       — territory bind list
//
// A-4b (IDENTITY-DESIGN.md §9.8, invariant I-26): a write of "kill" /
// "killgrp" to ctl terminates the target Proc's thread-group via
// proc_group_terminate. Authority is the two-axis I-26 set, enforced at the
// WRITE site (devproc.perm_enforced stays false; the shared open chokepoint
// hard-rejects pre-devproc.open, so the CAP_KILL axis cannot live at open):
// the caller must be the target's OWNER (same principal_id — ctl is 0600, so
// the owner always holds w) OR hold CAP_HOSTOWNER OR CAP_KILL. Checked
// directly, NOT via perm_check, so CAP_DAC_OVERRIDE (the fs-rwx admin) is not
// a kill axis — fs-admin and process-kill stay orthogonal. Containment is
// namespace visibility (I-1). USER-REACHABILITY of /proc is a Utopia
// namespace seam (devproc is kernel-internal at v1.0); the mechanism +
// authority here are kernel-unit-tested.
//
// Single-file leaves like /proc/<pid>/mem (raw memory access) and
// /proc/<pid>/fd/ (handle table directory) are held to later sub-chunks
// when the syscall surface + 9P readdir lands.
//
// dc='p'; first directory-typed Dev. Walks dispatch by current qid
// kind + name; the qid encoding pins the pid in upper 32 bits and a
// 4-bit subkind in the low byte.
//
// At v1.0 the proc table is walked via proc_find_by_pid (added in
// proc.c for this chunk). Lookup is O(N) DFS through the kproc tree;
// acceptable while the live-Proc count is bounded.

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// =============================================================================
// Qid path encoding.
// =============================================================================
//
// path = 0                         => root /proc directory (QTDIR).
// path = (pid << 32) | subkind     => per-pid object, subkind != 0.
//
// Subkinds form a closed set; PROC_QSUB_RESERVED == 0 marks the root.

enum {
    PQS_RESERVED = 0,        // reserved (root path = 0; never appears as a subkind in a /pid/ qid)
    PQS_PID_DIR  = 1,        // /proc/<pid>/                 (QTDIR)
    PQS_STATUS   = 2,        // /proc/<pid>/status           (QTFILE)
    PQS_CMDLINE  = 3,        // /proc/<pid>/cmdline          (QTFILE)
    PQS_CTL      = 4,        // /proc/<pid>/ctl              (QTFILE)
    PQS_NS       = 5,        // /proc/<pid>/ns               (QTFILE)
};

#define PROC_QID_ROOT_PATH  0ULL

static inline u64 proc_qid_make(int pid, u32 subkind) {
    return ((u64)(u32)pid << 32) | (u64)subkind;
}

static inline int proc_qid_pid(u64 path) {
    return (int)(u32)(path >> 32);
}

static inline u32 proc_qid_kind(u64 path) {
    return (u32)path;
}

// =============================================================================
// Per-pid file table (used by walk + read dispatch).
// =============================================================================

struct proc_pid_file {
    const char *name;
    u32         kind;
};

static const struct proc_pid_file g_proc_pid_files[] = {
    { "status",  PQS_STATUS  },
    { "cmdline", PQS_CMDLINE },
    { "ctl",     PQS_CTL     },
    { "ns",      PQS_NS      },
};

#define PROC_PID_FILE_COUNT \
    (sizeof(g_proc_pid_files) / sizeof(g_proc_pid_files[0]))

// =============================================================================
// Tiny formatters (no libc; inline for the synthetic-text fields).
// =============================================================================

// Append unsigned decimal `v` to buf at offset *off; cap-checked. Returns
// the number of bytes appended (always positive on success; 0 if there's
// not enough room).
static size_t fmt_udec(char *buf, size_t cap, size_t off, unsigned long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    if (off + (size_t)n > cap) return 0;
    for (int i = 0; i < n; i++) buf[off + i] = tmp[n - 1 - i];
    return (size_t)n;
}

// Append signed decimal.
static size_t fmt_sdec(char *buf, size_t cap, size_t off, long v) {
    if (v < 0) {
        if (off + 1 > cap) return 0;
        buf[off] = '-';
        size_t r = fmt_udec(buf, cap, off + 1, (unsigned long)(-v));
        return r ? 1 + r : 0;
    }
    return fmt_udec(buf, cap, off, (unsigned long)v);
}

static size_t fmt_str(char *buf, size_t cap, size_t off, const char *s) {
    size_t n = 0;
    while (s[n]) {
        if (off + n >= cap) return 0;
        buf[off + n] = s[n];
        n++;
    }
    return n;
}

// Translate proc_state to printable form.
static const char *state_name(enum proc_state s) {
    switch (s) {
    case PROC_STATE_INVALID: return "INVALID";
    case PROC_STATE_ALIVE:   return "ALIVE";
    case PROC_STATE_ZOMBIE:  return "ZOMBIE";
    default:                 return "?";
    }
}

// =============================================================================
// File-content generators. Each writes up to `cap` bytes; returns the
// number of bytes that would be produced for the WHOLE file (so the
// caller can compute the offset-aware copy).
// =============================================================================

// status: pid + state + threads + exit_status (if zombie).
static size_t format_status(struct Proc *p, char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    n = fmt_str(buf, cap, off, "pid:     ");      if (!n && off < cap) return 0; off += n;
    n = fmt_sdec(buf, cap, off, p->pid);          if (!n && p->pid != 0) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "state:   ");      if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, state_name(p->state)); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "threads: ");      if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off, p->thread_count); if (!n && p->thread_count != 0) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

    // #65 (I-32): the per-Proc resource-floor counters (the SEAM counters a
    // future aggregate quota reads). Atomic loads -- a cross-Proc reader holds
    // no per-Proc lock. page_count == live SYS_BURROW_ATTACH anon pages;
    // child_count == live direct children.
    {
        u32 pages = __atomic_load_n(&p->page_count, __ATOMIC_ACQUIRE);
        u32 kids  = __atomic_load_n(&p->child_count, __ATOMIC_ACQUIRE);
        n = fmt_str(buf, cap, off, "pages:   ");   if (!n) return 0; off += n;
        n = fmt_sdec(buf, cap, off, (int)pages);   if (!n && pages != 0) return 0; off += n;
        n = fmt_str(buf, cap, off, "\n");          if (!n) return 0; off += n;
        n = fmt_str(buf, cap, off, "children:");   if (!n) return 0; off += n;
        n = fmt_sdec(buf, cap, off, (int)kids);    if (!n && kids != 0) return 0; off += n;
        n = fmt_str(buf, cap, off, "\n");          if (!n) return 0; off += n;
    }

    if (p->state == PROC_STATE_ZOMBIE) {
        n = fmt_str(buf, cap, off, "exit:    ");   if (!n) return 0; off += n;
        n = fmt_sdec(buf, cap, off, p->exit_status); if (!n && p->exit_status != 0) return 0; off += n;
        n = fmt_str(buf, cap, off, "\n");          if (!n) return 0; off += n;
    }
    return off;
}

// cmdline: argv[0]. v1.0 has no argv yet — emit the special-case names
// for kproc / joey / generic, mirroring the boot banner. Phase 5+ fills
// from p->argv when ELF loader populates it.
static size_t format_cmdline(struct Proc *p, char *buf, size_t cap) {
    const char *name;
    if (p == kproc())          name = "kproc";
    else                        name = "<unnamed>";
    size_t off = 0;
    size_t n = fmt_str(buf, cap, off, name); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");        if (!n) return 0; off += n;
    return off;
}

// ns: report territory bind count (the territory's binds[] is private
// to territory.c; we just expose count + opaque path_id pairs would
// require a territory_iter API that's not yet there). v1.0: print the
// count.
static size_t format_ns(struct Proc *p, char *buf, size_t cap) {
    size_t off = 0;
    size_t n;
    n = fmt_str(buf, cap, off, "binds: ");                   if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off, territory_nbinds(p->territory));
    if (!n && territory_nbinds(p->territory) != 0)            return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");                         if (!n) return 0; off += n;
    return off;
}

// =============================================================================
// Decimal pid parser (devproc walk's "<pid>" name component).
// =============================================================================
//
// Returns true on a clean parse; rejects: empty string, leading '-' (no
// negative pids), non-digit chars, overflow past 31 bits.

static bool parse_decimal(const char *s, int *out) {
    if (!s || !*s) return false;
    unsigned long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 0x7fffffffUL) return false;
    }
    *out = (int)v;
    return true;
}

// =============================================================================
// Walk: one step from current qid following name.
// =============================================================================
//
// Returns true on success (filling *out_qid); false on miss. Special
// names: ".." goes up.

static bool walk_one(u64 cur_path, const char *name,
                     struct Qid *out_qid) {
    out_qid->path = 0;
    out_qid->vers = 0;
    out_qid->type = 0;
    out_qid->pad[0] = out_qid->pad[1] = out_qid->pad[2] = 0;

    // ".." — go up one level.
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        if (cur_path == PROC_QID_ROOT_PATH) {
            // root's parent is itself (the dev's root is the apex).
            out_qid->path = PROC_QID_ROOT_PATH;
            out_qid->type = QTDIR;
            return true;
        }
        // From any /proc/<pid>/ dir or file, ".." → root.
        out_qid->path = PROC_QID_ROOT_PATH;
        out_qid->type = QTDIR;
        return true;
    }

    // From root: name should be a decimal pid.
    if (cur_path == PROC_QID_ROOT_PATH) {
        int pid;
        if (!parse_decimal(name, &pid)) return false;
        struct Proc *p = proc_find_by_pid(pid);
        if (!p) return false;
        out_qid->path = proc_qid_make(pid, PQS_PID_DIR);
        out_qid->type = QTDIR;
        return true;
    }

    // From /proc/<pid>/: name should be one of the known files.
    u32 cur_kind = proc_qid_kind(cur_path);
    if (cur_kind == PQS_PID_DIR) {
        for (size_t i = 0; i < PROC_PID_FILE_COUNT; i++) {
            const char *f = g_proc_pid_files[i].name;
            // strcmp inline (no libc).
            int j = 0;
            while (f[j] && name[j] && f[j] == name[j]) j++;
            if (f[j] == 0 && name[j] == 0) {
                int pid = proc_qid_pid(cur_path);
                out_qid->path = proc_qid_make(pid, g_proc_pid_files[i].kind);
                out_qid->type = QTFILE;
                return true;
            }
        }
        return false;
    }

    // From a file qid: walking has no meaning. Plan 9 returns no progress.
    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devproc_reset(void)    { /* no-op */ }
static void devproc_init(void)     { /* no-op */ }
static void devproc_shutdown(void) { /* no-op */ }

static struct Spoor *devproc_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devproc, QTDIR);
    // dev_simple_attach sets qid.path = 0 (= PROC_QID_ROOT_PATH).
}

// Walk: support multi-step. Allocates a Walkqid sized for nname slots;
// walks each step via walk_one until first miss. Returns NULL only on
// allocation failure or invalid args; partial walks (nqid < nname)
// are returned with the partial Walkqid and the caller decides what
// to do.
//
// nc is ignored at v1.0 (always spoor_clone(c)). Phase 5+ may use it
// for in-place walk when 9P client integrates fid management.
static struct Walkqid *devproc_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)nc;
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    struct Spoor *cur = spoor_clone(c);
    if (!cur) {
        walkqid_free(wq);
        return NULL;
    }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!walk_one(cur->qid.path, name[i], &next)) {
            break;        // miss; stop and return what we have
        }
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devproc_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;        // 9P stat encoding lands when the syscall surface needs it
}

// Per-subkind mode (A-4b). ctl is 0600 (owner rw — the kill-authority gate);
// the info files are 0444 (world-readable, Plan 9 /proc convention); the pid
// dir is 0555.
static u32 devproc_mode_for_kind(u32 kind) {
    switch (kind) {
    case PQS_CTL:     return 0600u;
    case PQS_STATUS:
    case PQS_CMDLINE:
    case PQS_NS:      return 0444u;
    case PQS_PID_DIR: return 0555u;
    default:          return 0u;
    }
}

// stat_native — Thylacine fstat surface (A-4b). Reports the TARGET Proc's
// identity as the per-pid object's owner (uid/gid) with the per-subkind mode.
// devproc.perm_enforced stays false, so the open chokepoint does NOT consult
// this; it serves SYS_FSTAT introspection + documents the ownership model the
// ctl kill-authority (devproc_kill_authorized) enforces directly. Root (the
// dev apex) has no single owner -> -1.
//
// proc_find_by_pid here is lockless — the documented v1.0 "no concurrent
// reap" window, identical to devproc_read. The KILL path does NOT use this;
// it resolves the target under g_proc_table_lock via proc_for_each.
static int devproc_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out)                  return -1;
    u64 path = c->qid.path;
    if (path == PROC_QID_ROOT_PATH)  return -1;   // dev apex: no per-Proc owner
    u32 kind = proc_qid_kind(path);
    int pid  = proc_qid_pid(path);

    struct Proc *p = proc_find_by_pid(pid);
    if (!p)                          return -1;   // gone since walk

    u8 *z = (u8 *)out;
    for (size_t i = 0; i < sizeof(*out); i++) z[i] = 0;
    out->uid      = p->principal_id;
    out->gid      = p->primary_gid;
    out->mode     = devproc_mode_for_kind(kind);
    out->qid_path = path;
    out->qid_type = (kind == PQS_PID_DIR) ? QTDIR : QTFILE;
    return 0;
}

static struct Spoor *devproc_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devproc_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    // /proc is a synthetic FS — create returns NULL (SYS_WALK_CREATE -> -1).
    return NULL;
}

static void devproc_close(struct Spoor *c) {
    dev_simple_close(c);
}

// Read: dispatch by qid kind. Generates synthetic content into a stack
// buffer, then copies the requested [off, off+n) slice.
//
// Buffer cap is large enough for the v1.0 fields (status fits in <128 B
// even with longest pid; cmdline + ns are smaller). 256 B is generous.
#define DEVPROC_READ_BUF 256

static long devproc_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (off < 0) return -1;

    u32 kind = proc_qid_kind(c->qid.path);
    int  pid = proc_qid_pid(c->qid.path);

    char content[DEVPROC_READ_BUF];
    size_t total = 0;

    // Root + pid_dir reads return 0 (directories — readdir lands at
    // P4-D / 9P readdir). At v1.0 P4-C we don't synthesize directory
    // listings; reads on a directory qid get -1 to signal "not a
    // file". The future readdir path will replace this with a 9P
    // stat-record stream.
    if (c->qid.path == PROC_QID_ROOT_PATH || kind == PQS_PID_DIR) {
        return -1;
    }

    struct Proc *p = proc_find_by_pid(pid);
    if (!p) return -1;            // process gone since walk

    switch (kind) {
    case PQS_STATUS:
        total = format_status(p, content, sizeof(content));
        break;
    case PQS_CMDLINE:
        total = format_cmdline(p, content, sizeof(content));
        break;
    case PQS_NS:
        total = format_ns(p, content, sizeof(content));
        break;
    case PQS_CTL:
        // ctl is write-only at v1.0. Reads return empty (Plan 9: ctl is
        // for commands; no readable state).
        return 0;
    default:
        return -1;
    }

    if ((size_t)off >= total) return 0;            // EOF
    size_t avail = total - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;

    u8 *out = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) out[i] = (u8)content[(size_t)off + i];
    return (long)copy;
}

static struct Block *devproc_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// =============================================================================
// ctl kill — cross-process termination (A-4b; IDENTITY-DESIGN.md §9.8, I-26).
// =============================================================================

// Two-axis authority for a kill/killgrp write to /proc/<pid>/ctl. ctl is owned
// by the target's principal/group at mode 0600, so:
//   - identity axis: the OWNER (same principal_id — the owner always holds the
//     0600 w-bit) may kill (covers killing your own processes; the
//     parent-of-same-identity-child case is expressible as ownership);
//   - capability axis: CAP_HOSTOWNER (the unified admin) OR CAP_KILL (the
//     cross-identity override) may kill any target.
// Checked DIRECTLY (not via perm_check): CAP_DAC_OVERRIDE — the generic fs-rwx
// admin — is deliberately NOT a kill axis. The A-4 capability split keeps
// fs-admin and process-kill orthogonal (mirrors Linux CAP_DAC_OVERRIDE vs
// CAP_KILL). No identity bypasses (I-22 — the cap axes are capabilities, never
// identities). Non-static: the kernel test suite exercises the predicate.
bool devproc_kill_authorized(const struct Proc *caller, const struct Proc *target) {
    if (!caller || !target)                            return false;
    if (caller->principal_id == target->principal_id)  return true;   // owner-rwx on 0600
    // caps read ATOMICALLY (RW-5 F2): proc_become_legate is a cross-thread writer
    // of caller->caps since A-4a; a plain load is C11-racy (CAP_KILL is clearance-grantable).
    if (__atomic_load_n(&caller->caps, __ATOMIC_ACQUIRE) & (CAP_HOSTOWNER | CAP_KILL))
        return true;   // admin OR kill-anyone
    return false;
}

// ctl verbs. v1.0: kill / killgrp both terminate the target Proc's thread-
// group (no cross-Proc process groups at v1.0 — a distinct killgrp is a v1.x
// seam). stop / start are scheduler integration (ARCH OPEN Q 7.6.D); other
// tokens are rejected.
enum ctl_verb { CTL_VERB_NONE, CTL_VERB_KILL, CTL_VERB_KILLGRP, CTL_VERB_OTHER };

// Match the token [s, s+len) against a NUL-terminated literal (no libc).
static bool ctl_tok_eq(const char *s, long len, const char *lit) {
    long i = 0;
    while (lit[i]) { if (i >= len || s[i] != lit[i]) return false; i++; }
    return i == len;
}

// Parse the leading whitespace-delimited verb token from a ctl write.
static enum ctl_verb parse_ctl_verb(const char *s, long n) {
    if (!s || n <= 0) return CTL_VERB_NONE;
    long i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    long start = i;
    while (i < n && s[i] != ' '  && s[i] != '\t' && s[i] != '\n' &&
                    s[i] != '\r' && s[i] != '\0') i++;
    long len = i - start;
    if (len == 0)                              return CTL_VERB_NONE;
    if (ctl_tok_eq(s + start, len, "kill"))    return CTL_VERB_KILL;
    if (ctl_tok_eq(s + start, len, "killgrp")) return CTL_VERB_KILLGRP;
    return CTL_VERB_OTHER;
}

// proc_for_each context for the kill walk. result: 0 = target pid not found
// (cb never matched), +1 = killed, -1 = found but denied / not-ALIVE.
struct devproc_kill_ctx {
    int          target_pid;
    struct Proc *caller;
    int          result;
};

// Resolve + authorize + terminate, all under g_proc_table_lock (proc_for_each
// holds it throughout). The target is ALIVE under the lock, so no reap-UAF;
// proc_group_terminate is safe under the lock (it takes only torpor / rendez /
// cs locks, all below g_proc_table_lock) — the audited postnote_walk_cb idiom
// (kernel/syscall.c). Both verbs dispatch via proc_group_terminate uniformly:
// post-#811 it is the only termination primitive whose death-wake is total (a
// single-thread target blocked in a non-notes sleep would not wake on a bare
// note post). Refuse non-ALIVE targets (no consumer; mirrors postnote R2-F9).
static int devproc_kill_walk_cb(struct Proc *target, void *arg) {
    struct devproc_kill_ctx *k = (struct devproc_kill_ctx *)arg;
    if (target->pid != k->target_pid)                return 0;   // keep walking
    // kproc (the kernel proc, pid 0) is unkillable -- terminating it would take
    // down the kernel. Refuse regardless of caller authority, BEFORE the
    // authority check (a CAP_KILL holder cannot kill the kernel either).
    if (target == kproc())                         { k->result = -1; return 1; }
    if (target->state != PROC_STATE_ALIVE)         { k->result = -1; return 1; }
    if (!devproc_kill_authorized(k->caller, target)) { k->result = -1; return 1; }
    proc_group_terminate(target, "killed");
    k->result = 1;
    return 1;
}

// Write: ctl parses kill / killgrp and terminates the target Proc's thread-
// group (A-4b). Writes to status / cmdline / ns / dirs, and unrecognized
// verbs, return -1. The verb is offset-agnostic (a control message, not a byte
// stream), so off is ignored.
static long devproc_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    if (!c)    return -1;
    if (n < 0) return -1;

    u32 kind = proc_qid_kind(c->qid.path);
    if (kind != PQS_CTL) return -1;

    enum ctl_verb v = parse_ctl_verb((const char *)buf, n);
    if (v != CTL_VERB_KILL && v != CTL_VERB_KILLGRP) return -1;

    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    struct devproc_kill_ctx k = {
        .target_pid = proc_qid_pid(c->qid.path),
        .caller     = t->proc,
        .result     = 0,
    };
    proc_for_each(devproc_kill_walk_cb, &k);
    return (k.result == 1) ? n : -1;
}

static long devproc_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devproc_remove(struct Spoor *c) {
    (void)c;
}

static int devproc_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devproc_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devproc = {
    .dc       = 'p',
    .name     = "proc",

    .reset    = devproc_reset,
    .init     = devproc_init,
    .shutdown = devproc_shutdown,

    .attach      = devproc_attach,
    .walk        = devproc_walk,
    .stat        = devproc_stat,
    .stat_native = devproc_stat_native,

    .open     = devproc_open,
    .create   = devproc_create,
    .close    = devproc_close,

    .read     = devproc_read,
    .bread    = devproc_bread,
    .write    = devproc_write,
    .bwrite   = devproc_bwrite,

    .remove   = devproc_remove,
    .wstat    = devproc_wstat,
    .power    = devproc_power,
};
