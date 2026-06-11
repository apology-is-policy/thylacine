// /ctl — kernel admin Dev (P4-D).
//
// Per ARCHITECTURE.md §9.4 + ROADMAP §6.1. Plan 9 idiom: synthetic Dev
// exposing kernel state as text-format files. Reads return current
// values; writes (commands) are deferred to Phase 5+.
//
// v1.0 P4-D files (flat single-level layout; nested /ctl/kernel/ etc.
// per ARCH §9.4 deferred to a follow-up sub-chunk that adds nested
// directory walk):
//
//   /ctl/procs          — list of all processes (PID + state + threads)
//   /ctl/memory         — physical memory stats (total/free/reserved)
//   /ctl/devices        — bestiary listing (dc + name per Dev)
//   /ctl/kernel-base    — KASLR kernel high VA base + offset + seed source
//   /ctl/sched          — scheduler stats (runnable count)
//
// dc='C' (uppercase to leave 'c' for cons + 'r' for random).
//
// Pattern mirrors devproc.c: qid-encoded directory, multi-step walk,
// per-leaf format generator, offset-aware read. The format generators
// are static per-leaf functions producing into a 512-byte stack buffer.

#include <thylacine/dev.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../arch/arm64/kaslr.h"
#include "../mm/phys.h"

// =============================================================================
// Qid encoding.
// =============================================================================
//
// path = 0                 => root /ctl directory (QTDIR)
// path = leaf_kind         => leaf file (QTFILE), kind != 0

enum {
    CTL_KIND_RESERVED   = 0,
    CTL_KIND_PROCS      = 1,
    CTL_KIND_MEMORY     = 2,
    CTL_KIND_DEVICES    = 3,
    CTL_KIND_KERNEL_BASE = 4,
    CTL_KIND_SCHED      = 5,
};

#define CTL_QID_ROOT_PATH  0ULL

// =============================================================================
// Tiny formatters (mirrors devproc.c; could be shared via a future
// header — duplicated at v1.0 for chunk independence).
// =============================================================================

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

static size_t fmt_sdec(char *buf, size_t cap, size_t off, long v) {
    if (v < 0) {
        if (off + 1 > cap) return 0;
        buf[off] = '-';
        size_t r = fmt_udec(buf, cap, off + 1, (unsigned long)(-v));
        return r ? 1 + r : 0;
    }
    return fmt_udec(buf, cap, off, (unsigned long)v);
}

// 0x-prefixed lower-case hex of a u64.
static size_t fmt_uhex(char *buf, size_t cap, size_t off, u64 v) {
    char tmp[16];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(tmp)) {
            int d = (int)(v & 0xF);
            tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
            v >>= 4;
        }
    }
    if (off + 2 + (size_t)n > cap) return 0;
    buf[off + 0] = '0';
    buf[off + 1] = 'x';
    for (int i = 0; i < n; i++) buf[off + 2 + i] = tmp[n - 1 - i];
    return (size_t)(2 + n);
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

static const char *state_name(enum proc_state s) {
    switch (s) {
    case PROC_STATE_INVALID: return "INVALID";
    case PROC_STATE_ALIVE:   return "ALIVE";
    case PROC_STATE_ZOMBIE:  return "ZOMBIE";
    default:                 return "?";
    }
}

// =============================================================================
// Per-leaf content generators.
// =============================================================================

// procs: column-aligned PID/state/threads listing.
struct procs_fmt_state {
    char  *buf;
    size_t cap;
    size_t off;
    bool   overflow;
};

static int format_procs_cb(struct Proc *p, void *arg) {
    struct procs_fmt_state *s = (struct procs_fmt_state *)arg;
    size_t n;

    n = fmt_sdec(s->buf, s->cap, s->off, p->pid);
    if (!n && p->pid != 0) { s->overflow = true; return 0; }
    s->off += n;

    n = fmt_str(s->buf, s->cap, s->off, "    ");
    if (!n) { s->overflow = true; return 0; }
    s->off += n;

    n = fmt_str(s->buf, s->cap, s->off, state_name(p->state));
    if (!n) { s->overflow = true; return 0; }
    s->off += n;

    n = fmt_str(s->buf, s->cap, s->off, "    ");
    if (!n) { s->overflow = true; return 0; }
    s->off += n;

    int tc = __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE);  // #65 F6
    n = fmt_sdec(s->buf, s->cap, s->off, tc);
    if (!n && tc != 0) { s->overflow = true; return 0; }
    s->off += n;

    // #65 (I-32): the resource-floor counters as two trailing columns (the SEAM
    // counters). Atomic loads -- a cross-Proc reader holds no per-Proc lock.
    n = fmt_str(s->buf, s->cap, s->off, "    ");
    if (!n) { s->overflow = true; return 0; }
    s->off += n;
    {
        u32 pages = __atomic_load_n(&p->page_count, __ATOMIC_ACQUIRE);
        n = fmt_sdec(s->buf, s->cap, s->off, (int)pages);
        if (!n && pages != 0) { s->overflow = true; return 0; }
        s->off += n;
    }

    n = fmt_str(s->buf, s->cap, s->off, "    ");
    if (!n) { s->overflow = true; return 0; }
    s->off += n;
    {
        u32 kids = __atomic_load_n(&p->child_count, __ATOMIC_ACQUIRE);
        n = fmt_sdec(s->buf, s->cap, s->off, (int)kids);
        if (!n && kids != 0) { s->overflow = true; return 0; }
        s->off += n;
    }

    n = fmt_str(s->buf, s->cap, s->off, "\n");
    if (!n) { s->overflow = true; return 0; }
    s->off += n;

    return 0;        // continue iteration
}

static size_t format_procs(char *buf, size_t cap) {
    size_t off = 0;
    size_t n;
    n = fmt_str(buf, cap, off, "PID    STATE      THREADS    PAGES    CHILDREN\n");
    if (!n) return 0;
    off += n;

    struct procs_fmt_state s = { buf, cap, off, false };
    proc_for_each(format_procs_cb, &s);
    return s.off;
}

static size_t format_memory(char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    n = fmt_str(buf, cap, off, "total:    "); if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, phys_total_pages()); off += n;
    n = fmt_str(buf, cap, off, " pages\n"); if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "free:     "); if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, phys_free_pages()); off += n;
    n = fmt_str(buf, cap, off, " pages\n"); if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "reserved: "); if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, phys_reserved_pages()); off += n;
    n = fmt_str(buf, cap, off, " pages\n"); if (!n) return 0; off += n;

    return off;
}

static size_t format_devices(char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    n = fmt_str(buf, cap, off, "DC  NAME\n"); if (!n) return 0; off += n;

    for (int i = 0; bestiary[i] != NULL; i++) {
        if (off + 5 > cap) break;
        buf[off++] = (char)bestiary[i]->dc;
        n = fmt_str(buf, cap, off, "   "); if (!n) break; off += n;
        n = fmt_str(buf, cap, off, bestiary[i]->name); if (!n) break; off += n;
        n = fmt_str(buf, cap, off, "\n"); if (!n) break; off += n;
    }
    return off;
}

static size_t format_kernel_base(char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    n = fmt_str(buf, cap, off, "kernel_base:  "); if (!n) return 0; off += n;
    n = fmt_uhex(buf, cap, off, kaslr_kernel_high_base()); off += n;
    n = fmt_str(buf, cap, off, "\n"); if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "kaslr_offset: "); if (!n) return 0; off += n;
    n = fmt_uhex(buf, cap, off, kaslr_get_offset()); off += n;
    n = fmt_str(buf, cap, off, "\n"); if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "seed_source:  "); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off,
                kaslr_seed_source_str(kaslr_get_seed_source())); off += n;
    n = fmt_str(buf, cap, off, "\n"); if (!n) return 0; off += n;

    return off;
}

static size_t format_sched(char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    n = fmt_str(buf, cap, off, "runnable: "); if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, (unsigned long)sched_runnable_count()); off += n;
    n = fmt_str(buf, cap, off, "\n"); if (!n) return 0; off += n;

    return off;
}

// =============================================================================
// Per-leaf table.
// =============================================================================

struct ctl_leaf {
    const char *name;
    u32         kind;
    size_t    (*fmt)(char *buf, size_t cap);
};

static const struct ctl_leaf g_ctl_leaves[] = {
    { "procs",       CTL_KIND_PROCS,       format_procs       },
    { "memory",      CTL_KIND_MEMORY,      format_memory      },
    { "devices",     CTL_KIND_DEVICES,     format_devices     },
    { "kernel-base", CTL_KIND_KERNEL_BASE, format_kernel_base },
    { "sched",       CTL_KIND_SCHED,       format_sched       },
};

#define CTL_LEAF_COUNT  (sizeof(g_ctl_leaves) / sizeof(g_ctl_leaves[0]))

// Lookup leaf by kind. Returns NULL if no match.
static const struct ctl_leaf *leaf_for_kind(u32 kind) {
    for (size_t i = 0; i < CTL_LEAF_COUNT; i++) {
        if (g_ctl_leaves[i].kind == kind) return &g_ctl_leaves[i];
    }
    return NULL;
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

    // ".." goes up. /ctl is single-level — root and any leaf both have
    // the dev's apex as their parent.
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        out_qid->path = CTL_QID_ROOT_PATH;
        out_qid->type = QTDIR;
        return true;
    }

    if (cur_path == CTL_QID_ROOT_PATH) {
        for (size_t i = 0; i < CTL_LEAF_COUNT; i++) {
            if (name_eq(g_ctl_leaves[i].name, name)) {
                out_qid->path = (u64)g_ctl_leaves[i].kind;
                out_qid->type = QTFILE;
                return true;
            }
        }
        return false;
    }

    // From a leaf qid: walk has no meaning at v1.0.
    return false;
}

// =============================================================================
// Vtable.
// =============================================================================

static void devctl_reset(void)    { /* no-op */ }
static void devctl_init(void)     { /* no-op */ }
static void devctl_shutdown(void) { /* no-op */ }

static struct Spoor *devctl_attach(const char *spec) {
    (void)spec;
    return dev_simple_attach(&devctl, QTDIR);
}

static struct Walkqid *devctl_walk(struct Spoor *c, struct Spoor *nc,
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
        if (!walk_one(cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }

    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int devctl_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devctl_open(struct Spoor *c, int omode) {
    return dev_simple_open(c, omode);
}

static struct Spoor *devctl_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devctl_close(struct Spoor *c) {
    dev_simple_close(c);
}

#define DEVCTL_READ_BUF 512

static long devctl_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (off < 0) return -1;

    // Reads on the root directory return -1 (readdir held — same
    // reasoning as devproc).
    if (c->qid.path == CTL_QID_ROOT_PATH) return -1;

    u32 kind = (u32)c->qid.path;
    const struct ctl_leaf *leaf = leaf_for_kind(kind);
    if (!leaf) return -1;

    char content[DEVCTL_READ_BUF];
    size_t total = leaf->fmt(content, sizeof(content));

    if ((size_t)off >= total) return 0;
    size_t avail = total - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;

    u8 *out = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) out[i] = (u8)content[(size_t)off + i];
    return (long)copy;
}

static struct Block *devctl_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

// Writes are rejected at v1.0 P4-D. Admin commands (e.g., "scrub now",
// "freeze allocator", scheduler-tunables) hold to Phase 5+ alongside
// the syscall surface that exposes /ctl/ to userspace operators.
static long devctl_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)c; (void)buf; (void)n; (void)off;
    return -1;
}

static long devctl_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devctl_remove(struct Spoor *c) {
    (void)c;
}

static int devctl_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devctl_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

struct Dev devctl = {
    .dc       = 'C',
    .name     = "ctl",

    .reset    = devctl_reset,
    .init     = devctl_init,
    .shutdown = devctl_shutdown,

    .attach   = devctl_attach,
    .walk     = devctl_walk,
    .stat     = devctl_stat,

    .open     = devctl_open,
    .create   = devctl_create,
    .close    = devctl_close,

    .read     = devctl_read,
    .bread    = devctl_bread,
    .write    = devctl_write,
    .bwrite   = devctl_bwrite,

    .remove   = devctl_remove,
    .wstat    = devctl_wstat,
    .power    = devctl_power,
};
