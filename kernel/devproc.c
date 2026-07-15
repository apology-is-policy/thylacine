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

#include "../arch/arm64/timer.h"   // 8a-1b-beta: timer_now_ns -- the stop-wait poll deadline
#include "../arch/arm64/mmu.h"     // 8a-1b-gamma: mmu_cross_proc_read/write -- /proc/<pid>/mem

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
    PQS_MEM      = 6,        // /proc/<pid>/mem              (QTFILE; 8a-1b-gamma, I-39)
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
    { "mem",     PQS_MEM     },
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

    int tc = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE);  // #65 F6: lockless reader
    n = fmt_str(buf, cap, off, "threads: ");      if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off, tc);              if (!n && tc != 0) return 0; off += n;
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

// ns: render the territory's mount list (mount-point name + source name) + the
// bind count, via territory_format_ns (#66). The renderer takes the territory's
// ns_lock to read mounts[] -- the entries + their ref-held, immutable Path
// strings are stable for the read -- so format_ns runs under BOTH g_proc_table_-
// lock (the #57a F2 envelope keeping p->territory alive) AND, briefly, ns_lock.
// That g_proc_table_lock -> ns_lock edge is acyclic (nothing under ns_lock takes
// g_proc_table_lock; mount/unmount/clone only spoor_ref / path_ref / defer the
// sleeping clunk outside the lock). Pre-#66 this read the bind COUNT locklessly
// (a single aligned-int read); the mount LIST is a pointer walk a concurrent
// unmount could free, so the lock is now load-bearing for the read.
static size_t format_ns(struct Proc *p, char *buf, size_t cap) {
    return (size_t)territory_format_ns(p->territory, buf, (u64)cap);
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
// Reuse-nc contract (stalk / sys_walk_open_handler + clone_walk_zero's
// mount-cross): a non-NULL nc is the caller's pre-clone and MUST be returned as
// wq->spoor -- a 0-element walk then returns nc unchanged with nqid == 0, the
// shape clone_walk_zero needs to cross the /proc mount (#57). nc == NULL is the
// legacy direct-call shape (the kernel-internal devproc tests). Same dual mode
// devramfs_walk adopted at 16b-gamma; without it a mounted devproc is
// unreachable through stalk (wq->spoor != nc -> reject).
static struct Walkqid *devproc_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    if (!c) return NULL;
    if (nname < 0) return NULL;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

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
    case PQS_MEM:     return 0600u;   // 8a-1b-gamma: owner-private (I-39-gated at the RW site)
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
// #57a F2: the owner snapshot runs INSIDE a proc_for_each callback (under
// g_proc_table_lock, the kill-path shape), so the target Proc cannot be
// unlinked + freed between the find and the field reads (proc_unlink_child
// requires the lock). Pre-#57a devproc was unmounted -> reachable only
// single-threaded; the mount + SYS_FSTAT on /proc/<pid> make this cross-Proc +
// concurrently-reapable, so the old "no concurrent reap" claim was FALSE for
// the cross-Proc case the mount enables (the bare-pointer-after-unlock deref
// was a UAF). A non-zero callback return early-stops at the matched pid.
struct devproc_stat_ctx {
    int  pid;
    bool found;
    u32  uid;
    u32  gid;
};
static int devproc_stat_cb(struct Proc *p, void *arg) {
    struct devproc_stat_ctx *s = (struct devproc_stat_ctx *)arg;
    if (p->pid != s->pid) return 0;          // continue
    s->found = true;
    s->uid   = p->principal_id;
    s->gid   = p->primary_gid;
    return 1;                                 // matched -> stop
}
static int devproc_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out)                  return -1;
    u64 path = c->qid.path;
    if (path == PROC_QID_ROOT_PATH)  return -1;   // dev apex: no per-Proc owner
    u32 kind = proc_qid_kind(path);
    int pid  = proc_qid_pid(path);

    struct devproc_stat_ctx s = { .pid = pid, .found = false, .uid = 0, .gid = 0 };
    proc_for_each(devproc_stat_cb, &s);           // snapshot the owner under the lock
    if (!s.found)                    return -1;   // gone since walk

    u8 *z = (u8 *)out;
    for (size_t i = 0; i < sizeof(*out); i++) z[i] = 0;
    out->uid      = s.uid;
    out->gid      = s.gid;
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

// The ctl-fd close hook's release walk (8a-1b; identity-matched across ALL
// Procs). A closing debug-owner ctl Spoor releases exactly the target it owns —
// matching by pointer identity is correct under pid reuse (a reused pid with a
// different debug_owner is untouched) and is a no-op if the target was already
// reaped (debug_owner never matches). Runs OUTSIDE g_proc_table_lock (see
// devproc_close). The attach/detach walk (devproc_debug_walk_cb) lives with the
// ctl-verb machinery below.
struct devproc_debug_release_ctx { struct Spoor *ctl; bool found; };
static int devproc_debug_release_cb(struct Proc *p, void *arg) {
    struct devproc_debug_release_ctx *r = (struct devproc_debug_release_ctx *)arg;
    if (p->debug_owner != r->ctl) return 0;        // keep walking
    p->debug_owner = NULL;                          // release
    proc_debug_resume(p);                           // 8a-1b-beta: a dead/detached debugger provably resumes its quarry (ReleaseSlot -> NoStrand)
    // 8a-1b-beta: resume threads parked on p's debugger rendez here.
    r->found = true;
    return 1;                                        // matched -> stop
}

static void devproc_close(struct Spoor *c) {
    // 8a-1b (I-39, DEBUG-FS §7.2): the handle-lifetime-tied stop ownership. If
    // this ctl Spoor holds a debug attach slot (CDEBUGOWNER), releasing the fd —
    // by explicit close, or by debugger death closing its handles at exit
    // (#68/#926) — releases the slot (and, from 8a-1b-beta, resumes the target),
    // so a dead/detached debugger provably never strands its quarry. CDEBUGOWNER
    // gates the walk so a plain kill-user ctl close skips it. Runs OUTSIDE
    // g_proc_table_lock: this hook is reached from handle_release_obj (SYS_CLOSE)
    // and the #68 last-thread-out close, both with g_proc_table_lock DROPPED, so
    // proc_for_each may take it (no recursion).
    if (c && (c->flag & CDEBUGOWNER)) {
        struct devproc_debug_release_ctx r = { .ctl = c, .found = false };
        proc_for_each(devproc_debug_release_cb, &r);
    }
    dev_simple_close(c);
}

// Read: dispatch by qid kind. Generates synthetic content into a stack
// buffer, then copies the requested [off, off+n) slice.
//
// Buffer cap. status fits in <128 B; cmdline is smaller. #66 made ns render the
// full mount list ("mount <pt> <src>\n" per entry, up to PGRP_MAX_MOUNTS=12). 512 B
// holds the common short-name boot layout; a single deep mountpoint/source name
// (each up to SYS_OPEN_PATH_MAX=1024) can exceed it, so the list truncates --
// cleanly, at a whole-line boundary (territory_format_ns audit F2), best-effort
// per I-33. Stack-safe. (Completeness for deep namespaces is a v1.x offset-aware
// multi-read.)
#define DEVPROC_READ_BUF 512

// #57a F2: format under g_proc_table_lock (the kill-path shape) so the target
// Proc cannot be unlinked + freed between the lookup and the field deref
// (proc_unlink_child requires the lock; under it a Proc is either linked-alive
// or already unlinked-not-found). A non-zero callback return early-stops at the
// matched pid (also bounding the lock-hold). g_proc_table_lock keeps p (and thus
// p->territory + p->handles) alive across the format.
//
// #66: format_ns now reads the territory's mount LIST (pointer-chasing
// mounts[].mp_path / .source->path), so it takes p->territory->ns_lock inside
// territory_format_ns -- a g_proc_table_lock -> ns_lock edge. It is ACYCLIC
// (nothing held under ns_lock takes g_proc_table_lock: mount/unmount/clone only
// spoor_ref / path_ref and defer the sleeping spoor_clunk to OUTSIDE ns_lock).
// The pre-#66 lockless `binds: N` read was sound only because it was a single
// aligned-int copy; a mount-list pointer walk a concurrent unmount could free
// genuinely needs the lock. No sleep / no allocation runs under either lock.
struct devproc_read_ctx {
    int     pid;
    u32     kind;
    char   *buf;
    size_t  cap;
    size_t  total;
    bool    found;
};
static int devproc_read_cb(struct Proc *p, void *arg) {
    struct devproc_read_ctx *r = (struct devproc_read_ctx *)arg;
    if (p->pid != r->pid) return 0;          // continue
    r->found = true;
    switch (r->kind) {
    case PQS_STATUS:  r->total = format_status(p, r->buf, r->cap);  break;
    case PQS_CMDLINE: r->total = format_cmdline(p, r->buf, r->cap); break;
    case PQS_NS:      r->total = format_ns(p, r->buf, r->cap);      break;
    case PQS_CTL:     r->total = 0;                                 break;  // write-only: empty read
    default:          break;                  // kind pre-validated by the caller
    }
    return 1;                                 // matched -> stop
}
// 8a-1b-gamma: /proc/<pid>/mem RW (I-39-gated, stopped-only). Defined after
// devproc_debug_authorized; forward-declared so devproc_read/devproc_write reach
// it. `off` is the target user VA (mem is VA-addressed); `buf` is the kernel
// staging buffer the syscall layer bounced.
static long devproc_mem_rw(struct Spoor *c, void *buf, long n, s64 off, bool is_write);

static long devproc_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (off < 0) return -1;

    u32 kind = proc_qid_kind(c->qid.path);
    int  pid = proc_qid_pid(c->qid.path);

    // mem is a VA-addressed cross-Proc read (I-39 + stopped-only), not a
    // formatted info file -- its own path (gamma), before the format dispatch.
    if (kind == PQS_MEM) return devproc_mem_rw(c, buf, n, off, false);

    // Root + pid_dir reads return -1 (directories -- readdir lands with 9P
    // readdir). ctl is write-only at v1.0 (reads return empty; Plan 9: ctl is
    // for commands). Validate the kind BEFORE the lookup so the callback only
    // formats the readable file kinds.
    if (c->qid.path == PROC_QID_ROOT_PATH || kind == PQS_PID_DIR) return -1;
    if (kind != PQS_STATUS && kind != PQS_CMDLINE && kind != PQS_NS &&
        kind != PQS_CTL) return -1;

    char content[DEVPROC_READ_BUF];
    struct devproc_read_ctx r = {
        .pid = pid, .kind = kind, .buf = content, .cap = sizeof(content),
        .total = 0, .found = false,
    };
    proc_for_each(devproc_read_cb, &r);
    if (!r.found) return -1;                  // process gone since walk

    size_t total = r.total;
    if ((size_t)off >= total) return 0;       // EOF
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

// =============================================================================
// I-39 debug authority (8a-1b; docs/DEBUG-FS-DESIGN.md §3, the I-26 analog).
// =============================================================================
//
// Two-axis authority for the /proc/<pid> debug surface (attach + the mem/regs/
// wait reads in later sub-chunks), the I-26 kill-gate analog. ctl is 0600
// (owner rw), so:
//   - identity axis: the OWNER (same principal_id) may debug its own target;
//   - capability axis: CAP_HOSTOWNER (the host owner / Plan 9 "eve" — user-voted
//     a debug axis 2026-07-15: it already kills/chowns/DAC-overrides any target,
//     and debug is strictly less invasive than kill) OR CAP_DEBUG (the
//     clearance-grantable cross-identity debug authority) may debug any nameable
//     target.
// Checked DIRECTLY (not via perm_check): CAP_DAC_OVERRIDE — the generic fs-rwx
// admin — is deliberately NOT a debug axis, exactly as the kill gate keeps it
// off the kill axis (fs-admin stays orthogonal to debug). No identity bypasses
// (I-22 — the cap axes are capabilities, never identities). kproc (debugging it
// would stop the kernel) and a PROC_FLAG_NOTRACE target (the SYS_SET_TRACEABLE(0)
// no-trace seam — e.g. the login session Proc, DEBUG-FS-DESIGN §8) are refused
// BEFORE the authority axes: no cap holder can debug either. Non-static: the
// kernel test suite exercises the predicate.
bool devproc_debug_authorized(const struct Proc *caller, const struct Proc *target) {
    if (!caller || !target)                            return false;
    if (target == kproc())                             return false;   // kernel: undebuggable
    // NOTRACE is a monotonic one-way bit; a RELAXED read matches the setter
    // (sys_set_traceable) and is sound (it is set at target startup, before any
    // debugger could race — a stale-clear window cannot outlive the setter).
    if (__atomic_load_n(&target->proc_flags, __ATOMIC_RELAXED) & PROC_FLAG_NOTRACE)
        return false;                                                   // no-trace seam
    if (caller->principal_id == target->principal_id)  return true;    // owner-rwx on 0600
    // caps read ATOMICALLY (RW-5 F2): proc_become_legate is a cross-thread writer
    // of caller->caps; a plain load is C11-racy (both axes are clearance-grantable).
    if (__atomic_load_n(&caller->caps, __ATOMIC_ACQUIRE) & (CAP_HOSTOWNER | CAP_DEBUG))
        return true;                                                    // host owner OR debug-anyone
    return false;
}

// =============================================================================
// 8a-1b-gamma: /proc/<pid>/mem -- cross-Proc user-memory RW (I-39; DEBUG-FS 4.5).
// =============================================================================

// True iff every non-EXITING thread of `target` is parked on its OWN debug_rendez
// with on_cpu==false -- the "fully stopped" predicate a coherent cross-Proc
// mem/reg access relies on (specs/debug_stop.tla NoEL0AfterStopped). Caller holds
// g_proc_table_lock (the p->threads walk is stable under it -- the only mutator,
// thread_free, holds it); reads rendez_blocked_on under each peer's wait_lock (the
// same lock the park's register-then-observe takes, so it never sees a thread
// about to proceed to EL0), then the #788 spin-until-off_cpu read (a thread
// mid-cpu_switch_context still reads on_cpu==true until it fully deschedules).
static bool devproc_all_threads_parked(struct Proc *target) {
    for (struct Thread *peer = target->threads; peer; peer = peer->next_in_proc) {
        if (peer->state == THREAD_EXITING) continue;   // dying -- need not park
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        bool parked = (peer->rendez_blocked_on == &peer->debug_rendez);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
        if (!parked) return false;
        if (__atomic_load_n(&peer->on_cpu, __ATOMIC_ACQUIRE)) return false;   // still on-cpu
    }
    return true;
}

// The mem/reg access gate (I-39 + stopped-only, DEBUG-FS 3): the target is ALIVE,
// a debugger stop is pending, AND every thread is parked. Caller holds
// g_proc_table_lock. (A ZOMBIE/dying target is refused -- its pgtable_root may be
// torn down; its memory is meaningless to inspect.)
static bool devproc_target_fully_stopped(struct Proc *target) {
    if (target->state != PROC_STATE_ALIVE) return false;
    if (__atomic_load_n(&target->debug_stop_req, __ATOMIC_ACQUIRE) == 0) return false;
    return devproc_all_threads_parked(target);
}

// The g_proc_table_lock hold spans the resolve + gate + walk + copy, so the
// per-op copy is clamped: the debugger loops for a larger range (a rare debug
// path, not a hot one). One page keeps the global-lock hold to a page-copy.
#define DEBUG_MEM_CHUNK  ((long)4096)

struct devproc_mem_ctx {
    int          target_pid;
    struct Proc *caller;
    u64          vaddr;      // the target user VA (= the Dev off)
    void        *kbuf;       // the kernel staging buffer (syscall-bounced)
    long         len;        // clamped to <= DEBUG_MEM_CHUNK
    bool         is_write;
    long         result;     // >=0 bytes moved (short at a hole/RO), -1 denied/not-found
};

// Resolve + I-39 gate + stopped-only gate + the cross-Proc copy, all under
// g_proc_table_lock (proc_for_each). mmu_cross_proc_read/write take no sleeping
// lock (a direct-map memcpy), and target->vma_lock is a leaf below
// g_proc_table_lock (acyclic: nothing under vma_lock takes g_proc_table_lock) --
// so the copy is safe under the lock, which also pins the target ALIVE across it
// (no reap-UAF of pgtable_root).
static int devproc_mem_walk_cb(struct Proc *target, void *arg) {
    struct devproc_mem_ctx *m = (struct devproc_mem_ctx *)arg;
    if (target->pid != m->target_pid) return 0;   // keep walking
    if (target == kproc())                              { m->result = -1; return 1; }
    if (!devproc_debug_authorized(m->caller, target))   { m->result = -1; return 1; }  // I-39
    if (!devproc_target_fully_stopped(target))          { m->result = -1; return 1; }  // stopped-only

    irq_state_t vs = spin_lock_irqsave(&target->vma_lock);
    m->result = m->is_write
        ? mmu_cross_proc_write(target->pgtable_root, m->vaddr, m->kbuf, m->len)
        : mmu_cross_proc_read(target->pgtable_root,  m->vaddr, m->kbuf, m->len);
    spin_unlock_irqrestore(&target->vma_lock, vs);
    return 1;
}

static long devproc_mem_rw(struct Spoor *c, void *buf, long n, s64 off, bool is_write) {
    if (!c || !buf || n < 0) return -1;
    if (off < 0)             return -1;   // VA is non-negative (TTBR0 user-half)
    if (n == 0)              return 0;
    if (n > DEBUG_MEM_CHUNK) n = DEBUG_MEM_CHUNK;   // clamp; the debugger loops

    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    struct devproc_mem_ctx m = {
        .target_pid = proc_qid_pid(c->qid.path),
        .caller     = t->proc,
        .vaddr      = (u64)off,
        .kbuf       = buf,
        .len        = n,
        .is_write   = is_write,
        .result     = -1,
    };
    proc_for_each(devproc_mem_walk_cb, &m);
    return m.result;   // -1 not-found / denied; else bytes moved (0 = a hole at off)
}

// ctl verbs. v1.0: kill / killgrp both terminate the target Proc's thread-
// group (no cross-Proc process groups at v1.0 — a distinct killgrp is a v1.x
// seam). stop / start are scheduler integration (ARCH OPEN Q 7.6.D); other
// tokens are rejected.
enum ctl_verb {
    CTL_VERB_NONE, CTL_VERB_KILL, CTL_VERB_KILLGRP,
    CTL_VERB_ATTACH,   // 8a-1b-alpha: claim the one-debugger slot (I-39-gated; Einuse)
    CTL_VERB_DETACH,   // 8a-1b-alpha: release the slot (owner-fd only) + resume (beta)
    CTL_VERB_STOP,     // 8a-1b-beta: park the target's threads; block until stopped/exit
    CTL_VERB_START,    // 8a-1b-beta: resume the target (non-blocking)
    CTL_VERB_WAITSTOP, // 8a-1b-beta: block until the target is stopped/exits
    CTL_VERB_OTHER,
};

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
    if (ctl_tok_eq(s + start, len, "attach"))   return CTL_VERB_ATTACH;
    if (ctl_tok_eq(s + start, len, "detach"))   return CTL_VERB_DETACH;
    if (ctl_tok_eq(s + start, len, "stop"))     return CTL_VERB_STOP;
    if (ctl_tok_eq(s + start, len, "start"))    return CTL_VERB_START;
    if (ctl_tok_eq(s + start, len, "waitstop")) return CTL_VERB_WAITSTOP;
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

// =============================================================================
// ctl attach / detach — the one-debugger slot (8a-1b; I-39, DEBUG-FS §7.2).
// =============================================================================
//
// The debug attach slot is p->debug_owner: the /proc/<pid>/ctl Spoor that holds
// the debugger's claim (an identity token only — never dereferenced). Guarded
// by g_proc_table_lock; all of attach / detach / the ctl-fd-close release run
// under proc_for_each, so the claim + Einuse check + release are serialized (no
// atomic needed). This is the FOUNDATION: 8a-1b-beta makes the release ALSO
// resume threads parked on the debugger rendez (the model's ReleaseSlot wake) —
// at v1.0-alpha nothing parks yet, so release is a bare slot clear.

// proc_for_each context for the attach/detach walk. result: 0 = target pid not
// found, +1 = success, -1 = denied / Einuse / not-ALIVE / not-the-owner.
struct devproc_debug_ctx {
    int           target_pid;
    struct Proc  *caller;
    struct Spoor *ctl;         // the caller's ctl Spoor — the attach slot token
    enum ctl_verb verb;        // CTL_VERB_ATTACH or CTL_VERB_DETACH
    int           result;
};

static int devproc_debug_walk_cb(struct Proc *target, void *arg) {
    struct devproc_debug_ctx *d = (struct devproc_debug_ctx *)arg;
    if (target->pid != d->target_pid) return 0;   // keep walking

    if (d->verb == CTL_VERB_ATTACH) {
        // Refuse a non-ALIVE target, then the I-39 gate (kproc / NOTRACE /
        // owner-or-CAP_DEBUG), then Einuse: a non-NULL slot is already claimed.
        if (target->state != PROC_STATE_ALIVE)            { d->result = -1; return 1; }
        if (!devproc_debug_authorized(d->caller, target)) { d->result = -1; return 1; }
        if (target->debug_owner != NULL)                  { d->result = -1; return 1; }  // Einuse
        target->debug_owner = d->ctl;              // claim (under g_proc_table_lock)
        d->ctl->flag |= CDEBUGOWNER;               // gate the close-hook release
        d->result = 1;
        return 1;
    }

    // CTL_VERB_DETACH: release iff THIS ctl Spoor owns the slot (identity match,
    // so a stranger's detach or a stale post-reap detach is a clean -1 / no-op).
    if (target->debug_owner == d->ctl) {
        target->debug_owner = NULL;                // release
        proc_debug_resume(target);                 // 8a-1b-beta: clear the stop + wake parked threads (ReleaseSlot -> NoStrand)
        d->result = 1;
    } else {
        d->result = -1;
    }
    return 1;
}

// =============================================================================
// ctl run-control -- stop / start / waitstop (8a-1b-beta; I-39, DEBUG-FS §4.3).
// =============================================================================
//
// Run-control verbs require the caller's ctl fd to OWN the attach slot
// (target->debug_owner == c) -- attach first (I-39-gated), then control. This
// slot-ownership gate is STRICTER than the attach gate: only the attached
// debugger drives the run state, so a stranger who could attach (but hasn't)
// cannot stop/start/resume a target another debugger owns.

enum debug_runctl { DBG_RC_STOP, DBG_RC_START, DBG_RC_WAITSTOP };

struct devproc_runctl_ctx {
    int                target_pid;
    struct Spoor      *ctl;      // must == target->debug_owner (slot ownership)
    enum debug_runctl  op;
    int                result;   // 0 not found, +1 ok, -1 not-owner
};

// Resolve + slot-owner-check + (STOP) deliver / (START) resume, all under
// g_proc_table_lock (proc_for_each holds it). proc_debug_stop_deliver /
// proc_debug_resume take no sleeping lock (a flag store + smp_resched_others /
// a per-peer wait_lock wake), so both are safe under the lock -- the audited
// devproc_kill_walk_cb idiom. WAITSTOP mutates nothing here (the block is
// outside the lock, in devproc_debug_wait_stopped).
static int devproc_runctl_walk_cb(struct Proc *target, void *arg) {
    struct devproc_runctl_ctx *rc = (struct devproc_runctl_ctx *)arg;
    if (target->pid != rc->target_pid) return 0;   // keep walking
    if (target->debug_owner != rc->ctl) { rc->result = -1; return 1; }  // not the slot owner
    switch (rc->op) {
        case DBG_RC_STOP:     proc_debug_stop_deliver(target); break;
        case DBG_RC_START:    proc_debug_resume(target);       break;
        case DBG_RC_WAITSTOP: break;   // no mutation; the block is outside the lock
    }
    rc->result = 1;
    return 1;
}

// Scan a target (by pid) for full debug-stop. state: -1 gone/not-found, 0
// not-yet-stopped, 1 fully stopped, 2 slot released (debug_owner != ctl). "Fully
// stopped" = every non-EXITING thread is parked on its OWN debug_rendez with
// on_cpu==false (the #788 discipline: a thread mid-cpu_switch_context still reads
// on_cpu==true until it fully deschedules -- required so a later mem/reg read
// (gamma) sees a settled frame). rendez_blocked_on is read under the peer's
// wait_lock (the same lock the park's register-then-observe takes), so this
// never confirms a thread that is about to proceed to EL0 (NoLostStop). A target
// with a syscall-sleeping thread never reaches state 1 until that thread hits a
// checkpoint (the v1.0 non-preemptive stop).
struct devproc_stopscan_ctx {
    int           target_pid;
    struct Spoor *ctl;
    int           state;
};
static int devproc_stopscan_cb(struct Proc *target, void *arg) {
    struct devproc_stopscan_ctx *s = (struct devproc_stopscan_ctx *)arg;
    if (target->pid != s->target_pid) return 0;    // keep walking
    if (target->debug_owner != s->ctl) { s->state = 2; return 1; }  // slot released -- abort the wait
    s->state = devproc_all_threads_parked(target) ? 1 : 0;   // gamma-shared "fully parked" predicate
    return 1;
}

// tsleep cond for the stop-wait poll: never wakes on the cond (only the deadline
// does), so tsleep is a pure timed sleep between re-scans.
static int devproc_debug_poll_never(void *arg) { (void)arg; return 0; }

// Block the caller until the target (by pid) is fully debug-stopped, exits/is
// reaped, the slot is released, or the CALLER is death-interrupted. v1.0 uses a
// bounded re-poll (the edge-triggered /proc/<pid>/wait file is the gamma / v1.x
// upgrade -- but the poll re-scans under g_proc_table_lock so it can only be
// latent, never wrong). LIFETIME-SAFE: it re-resolves the target by pid each
// round (proc_for_each), holding NO target pointer across the lock drop, so a
// reaped target is simply not found. Sleeps on the caller's OWN stack rendez
// (single-waiter -- only the caller ever sleeps here); the deadline is the sole
// wake source. tsleep is death-interruptible, so a debugger killed while waiting
// unwinds (and its ctl-fd close then resumes the target). Returns +1 stopped,
// 0 gone / slot-released, -1 caller death-interrupted.
#define DEBUG_STOP_POLL_NS  (2ull * 1000ull * 1000ull)   // 2 ms between scans
static int devproc_debug_wait_stopped(int pid, struct Spoor *ctl) {
    struct Rendez pollr = RENDEZ_INIT;   // caller-private, single-waiter
    for (;;) {
        struct devproc_stopscan_ctx s = { .target_pid = pid, .ctl = ctl, .state = -1 };
        proc_for_each(devproc_stopscan_cb, &s);
        if (s.state == 1)  return 1;   // fully stopped
        if (s.state != 0)  return 0;   // -1 gone/reaped, or 2 slot released -- either ends the wait
        // state 0: not yet stopped -- sleep ~2 ms, then re-scan.
        u64 deadline = timer_now_ns() + DEBUG_STOP_POLL_NS;
        if (tsleep(&pollr, devproc_debug_poll_never, NULL, deadline) == TSLEEP_INTR)
            return -1;   // the caller's Proc is group-terminating -> unwind
    }
}

// Write: ctl parses kill / killgrp and terminates the target Proc's thread-
// group (A-4b), and the debug verbs attach / detach / stop / start / waitstop
// (8a-1b). Writes to status / cmdline / ns / dirs, and unrecognized verbs,
// return -1. The verb is offset-agnostic (a control message, not a byte
// stream), so off is ignored.
static long devproc_write(struct Spoor *c, const void *buf, long n, s64 off) {
    if (!c)    return -1;
    if (n < 0) return -1;

    u32 kind = proc_qid_kind(c->qid.path);
    // mem: a VA-addressed cross-Proc write (I-39 + stopped-only + W^X). `off` is
    // the target VA -- NOT ignored, unlike the ctl control message below.
    if (kind == PQS_MEM) return devproc_mem_rw(c, (void *)buf, n, off, true);
    (void)off;
    if (kind != PQS_CTL) return -1;

    enum ctl_verb v = parse_ctl_verb((const char *)buf, n);
    if (v != CTL_VERB_KILL && v != CTL_VERB_KILLGRP &&
        v != CTL_VERB_ATTACH && v != CTL_VERB_DETACH &&
        v != CTL_VERB_STOP && v != CTL_VERB_START && v != CTL_VERB_WAITSTOP) return -1;

    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    if (v == CTL_VERB_ATTACH || v == CTL_VERB_DETACH) {
        struct devproc_debug_ctx d = {
            .target_pid = proc_qid_pid(c->qid.path),
            .caller     = t->proc,
            .ctl        = c,
            .verb       = v,
            .result     = 0,
        };
        proc_for_each(devproc_debug_walk_cb, &d);
        return (d.result == 1) ? n : -1;
    }

    if (v == CTL_VERB_STOP || v == CTL_VERB_START || v == CTL_VERB_WAITSTOP) {
        int pid = proc_qid_pid(c->qid.path);
        enum debug_runctl op = (v == CTL_VERB_STOP)  ? DBG_RC_STOP  :
                               (v == CTL_VERB_START) ? DBG_RC_START : DBG_RC_WAITSTOP;
        // Resolve + slot-owner-check + (STOP) deliver / (START) resume under the
        // lock; a non-owner / not-found is -1.
        struct devproc_runctl_ctx rc = { .target_pid = pid, .ctl = c, .op = op, .result = 0 };
        proc_for_each(devproc_runctl_walk_cb, &rc);
        if (rc.result != 1) return -1;
        if (op == DBG_RC_START) return n;   // resume is non-blocking
        // stop / waitstop: block (outside the lock) until stopped / exit / slot
        // release; -1 only if the CALLER was death-interrupted.
        return (devproc_debug_wait_stopped(pid, c) >= 0) ? n : -1;
    }

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
