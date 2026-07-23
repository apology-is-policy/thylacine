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
#include <thylacine/joey.h>   // 8a-2c F2: boot_is_complete() -- gate hwverify to the boot window
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>  // prowl-3b: SCHED_BAND_* -- /proc/<pid>/sched band names
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/timer.h"        // 8a-1b-beta: timer_now_ns -- the stop-wait poll deadline
#include "../arch/arm64/hwdebug.h"      // 8a-2a: the self-scoped EL0 hardware-breakpoint verify
#include "../arch/arm64/uaccess.h"      // 8a-2a: UACCESS_USER_VA_TOP -- the hwverify user-half VA bound
#include "../arch/arm64/mmu.h"          // 8a-1b-gamma: mmu_cross_proc_read/write -- /proc/<pid>/mem
#include "../arch/arm64/exception.h"    // 8a-1b-gamma-2: struct exception_context -- /proc/<pid>/regs
#include "../arch/arm64/halls.h"        // 8a-1b-gamma-3: halls_walk_kernel_frames/link_addr -- /proc/<pid>/kstack
#include "../arch/arm64/halls_symtab.h" // 8a-1b-gamma-3: halls_symbolize -- on-target kstack symbolization
#include "../arch/arm64/kaslr.h"        // 8a-1b-gamma-3: kaslr_get_offset -- kstack link-translation
#include "../mm/slub.h"                 // 8a-2b: kzalloc/kfree -- the lazy debug_hw table

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
    PQS_MEM      = 6,        // /proc/<pid>/mem              (QTFILE; 8a-1b-gamma-1, I-39)
    PQS_REGS     = 7,        // /proc/<pid>/regs             (QTFILE; 8a-1b-gamma-2, I-39)
    PQS_FPREGS   = 8,        // /proc/<pid>/fpregs           (QTFILE; 8a-1b-gamma-2, I-39)
    PQS_WAIT     = 9,        // /proc/<pid>/wait             (QTFILE; 8a-1b-gamma-3, I-39; RO, blocks until stopped)
    PQS_KREGS    = 10,       // /proc/<pid>/kregs            (QTFILE; 8a-1b-gamma-3, I-39; RO, kernel-side frame)
    PQS_KSTACK   = 11,       // /proc/<pid>/kstack           (QTFILE; 8a-1b-gamma-3, I-39; RO, symbolized kernel bt)
    PQS_SCHED    = 12,       // /proc/<pid>/sched            (QTFILE; prowl-3b; RO, OQ-4 owner-or-CAP_HOSTOWNER)
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
    { "regs",    PQS_REGS    },
    { "fpregs",  PQS_FPREGS  },
    { "wait",    PQS_WAIT    },
    { "kregs",   PQS_KREGS   },
    { "kstack",  PQS_KSTACK  },
    { "sched",   PQS_SCHED   },
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

// Append "0x" + minimal-width lowercase hex (no leading zeros). Returns bytes
// appended, or 0 if there is no room (8a-1b-gamma-3: the /proc/<pid>/kstack
// symbolized backtrace).
static size_t fmt_hex(char *buf, size_t cap, size_t off, u64 v) {
    char tmp[18];   // "0x" + up to 16 hex digits
    tmp[0] = '0'; tmp[1] = 'x';
    int n = 2;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        char d[16];
        int m = 0;
        while (v && m < 16) { u64 x = v & 0xfu; d[m++] = (char)(x < 10u ? ('0' + x) : ('a' + (x - 10u))); v >>= 4; }
        while (m) tmp[n++] = d[--m];
    }
    if (off + (size_t)n > cap) return 0;
    for (int i = 0; i < n; i++) buf[off + i] = tmp[i];
    return (size_t)n;
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

// status: name + pid + state + threads + cpu_ns + ppid + principal/gid +
// pages/children + exit_status (if zombie). prowl-1 brought this to Plan 9
// parity (name + CPU time + parent + owner). PRECONDITION: called under
// g_proc_table_lock (via proc_for_each -> devproc_read_cb) -- proc_cpu_ns walks
// p->threads, and p->parent is read here, both stable only under that lock.
static size_t format_status(struct Proc *p, char *buf, size_t cap) {
    size_t off = 0;
    size_t n;

    // prowl-1: name first (Plan 9 puts it first). "?" for an unstamped Proc.
    n = fmt_str(buf, cap, off, "name:    ");      if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, p->name[0] ? p->name : "?"); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

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

    // prowl-1: cumulative on-CPU time (ns; the reader diffs it for %CPU) + the
    // parent pid + the owning principal/gid.
    u64 cpu_ns = proc_cpu_ns(p);                  // caller holds g_proc_table_lock
    n = fmt_str(buf, cap, off, "cpu_ns:  ");      if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, (unsigned long)cpu_ns); if (!n && cpu_ns != 0) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

    int ppid = p->parent ? p->parent->pid : 0;
    n = fmt_str(buf, cap, off, "ppid:    ");      if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off, ppid);            if (!n && ppid != 0) return 0; off += n;
    n = fmt_str(buf, cap, off, "\n");             if (!n) return 0; off += n;

    n = fmt_str(buf, cap, off, "principal:"); if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, (unsigned long)p->principal_id); if (!n && p->principal_id != 0) return 0; off += n;
    n = fmt_str(buf, cap, off, " gid:");          if (!n) return 0; off += n;
    n = fmt_udec(buf, cap, off, (unsigned long)p->primary_gid);  if (!n && p->primary_gid != 0) return 0; off += n;
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

// prowl-3b: /proc/<pid>/sched -- the per-thread scheduler-introspection block
// (band / cpu / run_ns / nsched / parks / migrations / state). The OQ-4
// deep-internals view: the summary (%cpu / state / mem) stays all-visible via
// /ctl/procs + /proc/<pid>/status, but this per-thread internal detail is gated
// at the READ site to owner-or-CAP_HOSTOWNER (devproc_sched_authorized).
// PRECONDITION: called under g_proc_table_lock (via devproc_read_cb) -- the
// p->threads walk + the per-thread reads are stable only under that lock
// (thread_free holds it; the #95 walk-safety proc_cpu_ns relies on the same).
static const char *sched_band_name(u32 band) {
    switch (band) {
    case SCHED_BAND_INTERACTIVE: return "INTR";
    case SCHED_BAND_NORMAL:      return "NORM";
    case SCHED_BAND_IDLE:        return "IDLE";
    default:                     return "?";
    }
}
static const char *thread_state_short(enum thread_state s) {
    switch (s) {
    case THREAD_RUNNING:  return "RUN";
    case THREAD_RUNNABLE: return "RDY";
    case THREAD_SLEEPING: return "SLP";
    case THREAD_EXITING:  return "EXIT";
    default:              return "?";
    }
}
static size_t format_sched(struct Proc *p, char *buf, size_t cap) {
    size_t off = 0, n;

    n = fmt_str(buf, cap, off, "name:    "); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, p->name[0] ? p->name : "?"); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, "\npid:     "); if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off, p->pid); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off, "\nthreads: "); if (!n) return 0; off += n;
    n = fmt_sdec(buf, cap, off,
                 __atomic_load_n(&p->thread_count, __ATOMIC_ACQUIRE)); if (!n) return 0; off += n;
    n = fmt_str(buf, cap, off,
                "\ntid band cpu run_ns nsched parks nmig state\n"); if (!n) return 0; off += n;

    // Per-thread rows, space-separated (the reader splits on whitespace). Bounded:
    // a row is written into a scratch offset `row` and only committed (off = row)
    // once the whole row + newline fit -- so a heavily-threaded Proc's tail is
    // cleanly truncated (like format_procs), never a partial row and never past
    // cap. EVERY per-thread field is read via a RELAXED __atomic load: the
    // counters against the switch-chokepoint writer (the run_ns pattern), and
    // state/band against the LOCK-FREE sched()-side writers (sched() /
    // sched_mark_interactive mutate a running peer's state/band without
    // g_proc_table_lock, so a plain read would be a C11 data race -- a benign
    // display-column read, but the atomic load documents it + is TSan-clean; the
    // sched_dump_runnable precedent; single-copy-atomic, never torn, a
    // mid-transition snapshot is acceptable for telemetry).
    for (struct Thread *t = p->threads; t; t = t->next_in_proc) {
        size_t row = off;
        u64 nsched = __atomic_load_n(&t->nsched, __ATOMIC_RELAXED);
        n = fmt_sdec(buf, cap, row, t->tid); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_str(buf, cap, row,
                    sched_band_name(__atomic_load_n(&t->band, __ATOMIC_RELAXED))); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        // cpu: "-" until dispatched (last_cpu is meaningful only once nsched > 0).
        if (nsched == 0) n = fmt_str(buf, cap, row, "-");
        else n = fmt_udec(buf, cap, row,
                          (unsigned long)__atomic_load_n(&t->last_cpu, __ATOMIC_RELAXED));
        if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_udec(buf, cap, row,
                     (unsigned long)__atomic_load_n(&t->run_ns, __ATOMIC_RELAXED)); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_udec(buf, cap, row, (unsigned long)nsched); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_udec(buf, cap, row,
                     (unsigned long)__atomic_load_n(&t->nsleeps, __ATOMIC_RELAXED)); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_udec(buf, cap, row,
                     (unsigned long)__atomic_load_n(&t->nmigrations, __ATOMIC_RELAXED)); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, " "); if (!n) break; row += n;
        n = fmt_str(buf, cap, row,
                    thread_state_short(__atomic_load_n(&t->state, __ATOMIC_RELAXED))); if (!n) break; row += n;
        n = fmt_str(buf, cap, row, "\n"); if (!n) break; row += n;
        off = row;   // commit the complete row
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

// 8a-2a: the /proc/<pid>/ctl READ. ctl is write-only (empty read) EXCEPT the
// self-scoped hardware-breakpoint verify result: if the READER is the Proc that
// armed a verify (a self-read of its own ctl -- current_thread()->proc == p),
// report "hwverify fired=<0|1> elr=0x..\n" so usr/hwbp-verify learns whether its
// EL0 breakpoint delivered EC 0x30. Any other reader (a cross-Proc ctl read)
// gets the empty write-only read -- no cross-Proc leak of the verify result.
static size_t format_ctl_read(struct Proc *p, char *buf, size_t cap) {
    struct Thread *t = current_thread();
    if (!t || t->proc != p) return 0;          // only the arming Proc reads its own result
    bool fired = false;
    u64  elr   = 0;
    if (!hwdebug_verify_result(p->pid, &fired, &elr)) return 0;   // no verify armed by p -> empty
    size_t off = 0, r;
    r = fmt_str(buf, cap, off, "hwverify fired="); if (!r) return 0; off += r;
    r = fmt_udec(buf, cap, off, fired ? 1UL : 0UL); if (!r) return 0; off += r;
    r = fmt_str(buf, cap, off, " elr=");           if (!r) return 0; off += r;
    r = fmt_hex(buf, cap, off, elr);               if (!r) return 0; off += r;
    r = fmt_str(buf, cap, off, "\n");              if (!r) return 0; off += r;
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
    case PQS_MEM:     return 0600u;   // 8a-1b-gamma-1: owner-private (I-39-gated at the RW site)
    case PQS_REGS:    return 0600u;   // 8a-1b-gamma-2: owner-private (I-39-gated at the RW site)
    case PQS_FPREGS:  return 0600u;
    case PQS_WAIT:    return 0400u;   // 8a-1b-gamma-3: RO notification (I-39-gated at the read site)
    case PQS_KREGS:   return 0400u;   // 8a-1b-gamma-3: RO kernel frame (I-39-gated at the read site)
    case PQS_KSTACK:  return 0400u;   // 8a-1b-gamma-3: RO symbolized bt (I-39-gated at the read site)
    case PQS_SCHED:   return 0400u;   // prowl-3b: RO deep internals (OQ-4-gated at the read site)
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
    hwdebug_bp_clear_all(p->debug_hw);              // 8a-2b-1: a dead debugger's breakpoints are disarmed (else the orphaned target re-traps forever)
    hwdebug_wp_clear_all(p->debug_hw);              // 8a-2b-3: likewise its watchpoints (else the orphaned target re-traps on the watched access forever)
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
// full mount list ("mount <pt> <src>\n" per entry, up to PGRP_MAX_MOUNTS=12).
// prowl-3b bumped 512 -> 2048: /proc/<pid>/sched formats one row per thread of a
// possibly heavily-threaded Proc (a Go binary / stratumd), each ~40-50 B; 512
// truncated at ~8 threads. 2048 holds ~30 thread rows (the format bounds the
// walk cleanly on overflow either way). A deep-namespace ns render (each name up
// to SYS_OPEN_PATH_MAX=1024) can still exceed it, truncating at a whole-line
// boundary (territory_format_ns audit F2), best-effort per I-33. A 2 KiB frame
// on the 16 KiB kstack is safe on the shallow Dev-read path (the DEVCTL_READ_BUF
// precedent). (Completeness for deep namespaces is a v1.x offset-aware multi-read.)
#define DEVPROC_READ_BUF 2048

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
// prowl-3b: forward-declared so devproc_read_cb (below) can apply the OQ-4 gate;
// the definition sits with the other authority predicates (after
// devproc_kill_authorized). Non-static -- the test suite exercises it.
bool devproc_sched_authorized(const struct Proc *caller, const struct Proc *target);
size_t devproc_sched_read_gated(const struct Proc *caller, struct Proc *target,
                                char *buf, size_t cap, bool *denied);

// prowl-3b (prowl-5 F4): the OQ-4-gated sched read, factored out so the unit
// suite can exercise the DENY wiring with a synthetic (caller, target) pair --
// the in-kernel test runner is always kproc (CAP_ALL, always CAP_HOSTOWNER), so
// the deny leg is otherwise unreachable in-unit and a wiring regression (drop the
// gate) would leave every test green. Sets *denied on an OQ-4 denial (caller
// neither owner nor CAP_HOSTOWNER), returns 0, and formats NOTHING (no partial
// leak); otherwise formats target's per-thread block. `target` is resolved +
// pinned alive under g_proc_table_lock by the real caller (devproc_read_cb).
// Non-static: test-driven.
size_t devproc_sched_read_gated(const struct Proc *caller, struct Proc *target,
                                char *buf, size_t cap, bool *denied) {
    if (!devproc_sched_authorized(caller, target)) { *denied = true; return 0; }
    *denied = false;
    return format_sched(target, buf, cap);
}

struct devproc_read_ctx {
    int                 pid;
    u32                 kind;
    char               *buf;
    size_t              cap;
    size_t              total;
    bool                found;
    // prowl-3b (OQ-4): the reading Proc (captured before proc_for_each) + the
    // gate verdict for a gated kind. `caller` is read-only identity/caps; the
    // target is `p` found under the lock, so the gate resolves both under the
    // lock and sets `denied` for the read() to translate to -1.
    const struct Proc  *caller;
    bool                denied;
};
static int devproc_read_cb(struct Proc *p, void *arg) {
    struct devproc_read_ctx *r = (struct devproc_read_ctx *)arg;
    if (p->pid != r->pid) return 0;          // continue
    r->found = true;
    switch (r->kind) {
    case PQS_STATUS:  r->total = format_status(p, r->buf, r->cap);  break;
    case PQS_CMDLINE: r->total = format_cmdline(p, r->buf, r->cap); break;
    case PQS_NS:      r->total = format_ns(p, r->buf, r->cap);      break;
    case PQS_CTL:     r->total = format_ctl_read(p, r->buf, r->cap); break;  // 8a-2a hwverify result; else empty
    case PQS_SCHED:                            // prowl-3b: OQ-4 owner-or-CAP_HOSTOWNER
        r->total = devproc_sched_read_gated(r->caller, p, r->buf, r->cap, &r->denied);
        break;
    default:          break;                  // kind pre-validated by the caller
    }
    return 1;                                 // matched -> stop
}
// 8a-1b-gamma: /proc/<pid>/mem RW (I-39-gated, stopped-only). Defined after
// devproc_debug_authorized; forward-declared so devproc_read/devproc_write reach
// it. `off` is the target user VA (mem is VA-addressed); `buf` is the kernel
// staging buffer the syscall layer bounced.
static long devproc_mem_rw(struct Spoor *c, void *buf, long n, s64 off, bool is_write);
// 8a-1b-gamma-2: /proc/<pid>/regs + fpregs RW (I-39-gated, stopped-only). Same
// forward-decl reason as devproc_mem_rw.
static long devproc_regs_rw(struct Spoor *c, void *buf, long n, s64 off, bool is_write);
// 8a-1b-gamma-3: /proc/<pid>/{kstack,wait} RO reads. devproc_read dispatches them
// (same forward-decl reason); both are DEFINED after the ctl run-control section,
// since devproc_wait_read reuses its bounded-poll machinery (DEBUG_STOP_POLL_NS /
// devproc_debug_poll_never).
static long devproc_kstack_read(struct Spoor *c, void *buf, long n, s64 off);
static long devproc_wait_read(struct Spoor *c, void *buf, long n, s64 off);

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
    if (kind == PQS_REGS || kind == PQS_FPREGS) return devproc_regs_rw(c, buf, n, off, false);
    // 8a-1b-gamma-3: kregs reuses the regs read path (devproc_build_regs builds a
    // t_kernel_regs for PQS_KREGS); kstack/wait are their own RO paths.
    if (kind == PQS_KREGS)  return devproc_regs_rw(c, buf, n, off, false);
    if (kind == PQS_KSTACK) return devproc_kstack_read(c, buf, n, off);
    if (kind == PQS_WAIT)   return devproc_wait_read(c, buf, n, off);

    // Root + pid_dir reads return -1 (directories -- readdir lands with 9P
    // readdir). ctl is write-only at v1.0 (reads return empty; Plan 9: ctl is
    // for commands). Validate the kind BEFORE the lookup so the callback only
    // formats the readable file kinds.
    if (c->qid.path == PROC_QID_ROOT_PATH || kind == PQS_PID_DIR) return -1;
    if (kind != PQS_STATUS && kind != PQS_CMDLINE && kind != PQS_NS &&
        kind != PQS_CTL && kind != PQS_SCHED) return -1;

    // prowl-3b (OQ-4): capture the reading Proc BEFORE the lock so the gated
    // PQS_SCHED kind can be authorized against the target found inside the walk.
    struct Thread *self = current_thread();
    struct Proc   *caller = self ? self->proc : NULL;

    char content[DEVPROC_READ_BUF];
    struct devproc_read_ctx r = {
        .pid = pid, .kind = kind, .buf = content, .cap = sizeof(content),
        .total = 0, .found = false, .caller = caller, .denied = false,
    };
    proc_for_each(devproc_read_cb, &r);
    if (!r.found) return -1;                  // process gone since walk
    if (r.denied) return -1;                  // OQ-4: not owner, no CAP_HOSTOWNER

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

// prowl-3b (PROWL-DESIGN.md OQ-4): the deep-internals visibility gate for
// /proc/<pid>/sched (the per-thread scheduler view). Owner-or-CAP_HOSTOWNER --
// the operator's-or-owner's view: the SUMMARY (name / %cpu / state / mem) stays
// all-visible via /ctl/procs + /proc/<pid>/status (the Plan 9 all-pids posture),
// but a process's deep per-thread scheduler internals are the owner's own or the
// host owner's. STRICTLY NARROWER than the kill / debug gates: CAP_KILL and
// CAP_DEBUG are deliberately NOT axes (reading scheduler telemetry is neither
// killing nor debugging -- keep the capability split orthogonal, I-22); no
// identity bypasses. Composes I-1 (a confined user sees full detail only on its
// own Procs) + the /ctl visibility-not-authority posture; NO new §28 invariant.
// Non-static: the kernel test suite exercises the predicate.
bool devproc_sched_authorized(const struct Proc *caller, const struct Proc *target) {
    if (!caller || !target)                            return false;
    if (caller->principal_id == target->principal_id)  return true;   // owner
    // caps read ATOMICALLY (RW-5 F2): proc_become_legate is a cross-thread writer
    // of caller->caps; CAP_HOSTOWNER is clearance-grantable, so a plain load is racy.
    if (__atomic_load_n(&caller->caps, __ATOMIC_ACQUIRE) & CAP_HOSTOWNER)
        return true;                                                   // host owner
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
//
// PTY-1f: DELIBERATELY reads debug_stop_req ONLY -- a job-stopped Proc
// (job_stop_req, the second stop owner) is NOT debugger-stopped: its threads
// park on the same debug_rendez, but the mem/regs/wait surface keys on the
// DEBUG owner axis (I-39's attach + stop protocol; a Ctrl-Z'd process must
// not become debugger-readable by whoever holds a ctl fd without issuing the
// debug stop). A debugger stopping an already-job-parked target sets
// debug_stop_req and finds the threads already parked -> fully-stopped
// immediately; correct and cheap. Do NOT generalize this read to
// proc_stop_requested.
static bool devproc_target_fully_stopped(struct Proc *target) {
    if (target->state != PROC_STATE_ALIVE) return false;
    if (__atomic_load_n(&target->debug_stop_req, __ATOMIC_ACQUIRE) == 0) return false;
    // 8a-1c holotype F1: a group-terminating target is DYING, not debug-stopped.
    // Its threads transition to THREAD_EXITING and run their exit path
    // (thread_exit_self -> the final sched(), which WRITES t->ctx WITHOUT
    // g_proc_table_lock -- proc.c). devproc_all_threads_parked SKIPS EXITING
    // peers, so without this a dying target reads "fully stopped" and the reg/
    // kstack readers (which read t->ctx / the head thread) would race the
    // non-settled dying thread (a torn read; bounded to no-UAF only because the
    // reader holds g_proc_table_lock, pinning the Thread alive). Death wins: a
    // target with a pending group_exit_msg is never fully-stopped. Serialized
    // with proc_group_terminate's set (both under g_proc_table_lock).
    if (__atomic_load_n(&target->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) return false;
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

// =============================================================================
// 8a-1b-gamma-2: /proc/<pid>/regs + fpregs -- the stopped target's saved EL0
// register frames (I-39; DEBUG-FS 4.5). Same gate as mem. v1.0 reads the FOCUS
// thread (the M at the stop; below) or, absent one, the HEAD thread (p->threads);
// a per-thread /proc/<pid>/thread/<tid>/ layer is v1.x.
// =============================================================================

// 8c-2 #95: the debug-fs FOCUS thread -- the M whose frame regs/kregs/kstack +
// the step verb report. proc_debug_fault_stop sets p->debug_focus_thread to the
// bp/step/wp-firing M (a Go breakpoint fires on whichever M runs the migrated
// goroutine, NOT the head TID==PID); a manual stop / attach leaves it NULL. The
// pointer is validated against the CURRENT p->threads under g_proc_table_lock
// (every caller holds it): NO UAF, and the load-bearing reason is the GPTL PIN
// (#95-audit F1), NOT a stop gate -- reap (wait_pid_for) does proc_unlink_child
// under this lock BEFORE the lock-free thread_free loop, and proc_for_each walks
// the kproc-rooted children tree, so a target a reader can still reach has not
// been unlinked -> none of its threads is freed. A pointer that matches an
// in-list thread is therefore a LIVE struct; this validation loop IS the safety
// net (do NOT delete it as "redundant" -- the 8b settled kstack drops the
// fully-stopped gate, so this loop is the only thing turning a stale/foreign focus
// into a safe head fallback). Any mismatch (NULL / a stale-uncleared / a foreign
// pointer) -> head; the frame's own gates (EXITING / on_cpu / a valid trapframe)
// are re-checked by each caller downstream. NON-static so the selection is
// unit-testable (#95-audit F3; test_devproc.c `devproc.focus_selection`).
struct Thread *devproc_focus_thread(struct Proc *target) {
    struct Thread *focus = __atomic_load_n(&target->debug_focus_thread, __ATOMIC_ACQUIRE);
    if (focus)
        for (struct Thread *th = target->threads; th; th = th->next_in_proc)
            if (th == focus)
                return focus;
    return target->threads;   // the head thread (the v1.0 manual-stop / attach default)
}

// Build the target focus thread's regs (the EL0 trapframe at the kstack top) or
// fpregs (t->ctx's saved FP/SIMD) into `out`. Returns the struct size, or 0 on
// failure (no head thread / no kstack). The trapframe is at kstack_base +
// kstack_size - EXCEPTION_CTX_SIZE (the outermost EL0->EL1 KERNEL_ENTRY frame;
// a debug-parked thread always entered from EL0, so it is valid). It is kernel
// memory (TTBR1) -- a plain deref, no cross-Proc walk. Caller holds
// g_proc_table_lock + has gated I-39 + fully-stopped (so on_cpu==false: the
// frame + ctx are settled, not being written by a live cpu_switch_context).
static long devproc_build_regs(struct Proc *target, u32 kind, u8 *out) {
    struct Thread *th = devproc_focus_thread(target);   // 8c-2 #95: the M at the stop (else head)
    // 8a-1c holotype F1 (defense-in-depth): never read an EXITING head thread's
    // ctx/trapframe -- it is running its exit path (the final sched() writes
    // t->ctx). The fully_stopped gate already rejects a group-terminating target;
    // this backstops any EXITING head not covered by group_exit_msg.
    if (!th || th->state == THREAD_EXITING) return 0;

    // 8a-1b-gamma-3: kregs reads t->ctx (the saved KERNEL-side callee-saved
    // frame), NOT the EL0 trapframe, so it needs only a valid thread -- no
    // kstack-size guard. The struct offsets mirror struct Context's GP region
    // (context.h), so this is a verbatim field copy. tpidr_el0 is the EL0 TLS
    // base dlv reads for the Go `g` (DEBUG-FS 4.5); ttbr0 is omitted (a kernel
    // pgtable PA -- info-leak, no debug value).
    if (kind == PQS_KREGS) {
        struct t_kernel_regs *kr = (struct t_kernel_regs *)out;
        kr->x[0] = th->ctx.x19; kr->x[1] = th->ctx.x20; kr->x[2] = th->ctx.x21;
        kr->x[3] = th->ctx.x22; kr->x[4] = th->ctx.x23; kr->x[5] = th->ctx.x24;
        kr->x[6] = th->ctx.x25; kr->x[7] = th->ctx.x26; kr->x[8] = th->ctx.x27;
        kr->x[9] = th->ctx.x28;
        kr->fp        = th->ctx.fp;
        kr->lr        = th->ctx.lr;
        kr->sp        = th->ctx.sp;
        kr->tpidr_el0 = th->ctx.tpidr_el0;
        return (long)sizeof(struct t_kernel_regs);
    }

    // regs/fpregs read the EL0 trapframe (regs) / the ctx FP block (fpregs) --
    // require a real kstack big enough for the outermost KERNEL_ENTRY frame.
    if (!th->kstack_base || th->kstack_size < EXCEPTION_CTX_SIZE) return 0;

    if (kind == PQS_REGS) {
        // 8a-1c: the EL0-entry trapframe is NOT at a fixed kstack offset (its base
        // is SP_EL1-at-entry - EXCEPTION_CTX_SIZE, which is only kstack_top-288 for
        // a thread's first entry; a running thread's outermost frame sits lower).
        // debug_trapframe was captured from the thread's vector ctx: at the
        // RETURN tail by el0_return_stop_check (a TAIL-parked thread), OR at the
        // EL0-sync ENTRY by exception_sync_lower_el (the #88 fix -- a DETOUR-parked
        // sleeper never reaches the tail while stopped, so its frame is the entry
        // frame). Read THAT. Reached ONLY when the target is fully-stopped (parked
        // on debug_rendez, either way), so it is set + fresh -- do NOT delete the
        // entry-store as redundant: it is the sole source for a syscall-blocked
        // detour-parked head. Validate it lies within the usable kstack (a NULL /
        // corrupt pointer -> no regs).
        struct exception_context *tf = th->debug_trapframe;
        if (!tf) return 0;
        u64 klo = (u64)(uintptr_t)th->kstack_base + (u64)THREAD_KSTACK_GUARD_SIZE;
        u64 ktop = (u64)(uintptr_t)th->kstack_base + (u64)th->kstack_size;
        u64 tfp  = (u64)(uintptr_t)tf;
        if (tfp < klo || tfp + (u64)EXCEPTION_CTX_SIZE > ktop) return 0;
        struct t_user_regs *ur = (struct t_user_regs *)out;
        for (int i = 0; i < 31; i++) ur->regs[i] = tf->regs[i];
        ur->sp     = tf->sp;      // SP_EL0
        ur->pc     = tf->elr;     // ELR_EL1
        ur->pstate = tf->spsr;    // SPSR_EL1 (read reflects it; write ignores it)
        return (long)sizeof(struct t_user_regs);
    }
    // PQS_FPREGS
    struct t_user_fpregs *uf = (struct t_user_fpregs *)out;
    for (int i = 0; i < 512; i++) uf->vregs[i] = th->ctx.fp_v[i];
    uf->fpsr = th->ctx.fpsr;
    uf->fpcr = th->ctx.fpcr;
    return (long)sizeof(struct t_user_fpregs);
}

// Apply an edited struct back to the target head thread. regs: x0..x30, sp
// (SP_EL0), pc (ELR_EL1). pstate (SPSR_EL1) is DELIBERATELY NOT written -- an
// arbitrary SPSR could eret the target to EL1 (privilege escalation), so the
// saved SPSR is kept (step-related SPSR.SS is 8a-2). fpregs: all fields (no
// privilege bits). Caller holds g_proc_table_lock + gated fully-stopped.
static void devproc_apply_regs(struct Proc *target, u32 kind, const u8 *in) {
    if (kind == PQS_KREGS) return;   // 8a-1b-gamma-3: kregs is RO (no write path routes here; defensive)
    struct Thread *th = devproc_focus_thread(target);   // 8c-2 #95: write the M at the stop (else head)
    if (!th || !th->kstack_base || th->kstack_size < EXCEPTION_CTX_SIZE) return;

    if (kind == PQS_REGS) {
        // 8a-1c: write the RECORDED trapframe (same pointer the read uses -- NOT
        // the fixed kstack_top-288 offset). Validated within the usable kstack.
        struct exception_context *tf = th->debug_trapframe;
        if (!tf) return;
        u64 klo = (u64)(uintptr_t)th->kstack_base + (u64)THREAD_KSTACK_GUARD_SIZE;
        u64 ktop = (u64)(uintptr_t)th->kstack_base + (u64)th->kstack_size;
        u64 tfp  = (u64)(uintptr_t)tf;
        if (tfp < klo || tfp + (u64)EXCEPTION_CTX_SIZE > ktop) return;
        const struct t_user_regs *ur = (const struct t_user_regs *)in;
        for (int i = 0; i < 31; i++) tf->regs[i] = ur->regs[i];
        tf->sp  = ur->sp;    // SP_EL0
        tf->elr = ur->pc;    // ELR_EL1 (resume PC; still EL0 -- SPSR unchanged)
        // tf->spsr LEFT UNCHANGED (the privilege guard).
        return;
    }
    const struct t_user_fpregs *uf = (const struct t_user_fpregs *)in;
    for (int i = 0; i < 512; i++) th->ctx.fp_v[i] = uf->vregs[i];
    th->ctx.fpsr = uf->fpsr;
    th->ctx.fpcr = uf->fpcr;
}

struct devproc_regs_ctx {
    int          target_pid;
    struct Proc *caller;
    u32          kind;      // PQS_REGS or PQS_FPREGS
    void        *kbuf;      // the kernel staging buffer
    long         n;
    s64          off;       // byte offset into the register struct
    bool         is_write;
    long         result;    // >=0 bytes moved (0 = off past EOF), -1 denied/not-found
};

static int devproc_regs_walk_cb(struct Proc *target, void *arg) {
    struct devproc_regs_ctx *r = (struct devproc_regs_ctx *)arg;
    if (target->pid != r->target_pid) return 0;
    if (target == kproc())                              { r->result = -1; return 1; }
    if (!devproc_debug_authorized(r->caller, target))   { r->result = -1; return 1; }  // I-39
    if (!devproc_target_fully_stopped(target))          { r->result = -1; return 1; }  // stopped-only

    // Build the current struct, apply the [off,off+n) slice (write) or copy it
    // out (read). The build-overlay-apply write path makes a PARTIAL write use
    // the current bytes for the untouched fields -- and the pstate guard holds
    // regardless of the write's offset (devproc_apply_regs never writes SPSR).
    u8 scratch[sizeof(struct t_user_fpregs)];   // the larger struct (520)
    long size = devproc_build_regs(target, r->kind, scratch);
    if (size == 0)       { r->result = -1; return 1; }   // no head thread / no kstack
    if (r->off >= size)  { r->result = 0;  return 1; }   // off past EOF -> 0
    long avail = size - (long)r->off;
    long cnt = (r->n < avail) ? r->n : avail;
    if (r->is_write) {
        for (long i = 0; i < cnt; i++) scratch[r->off + i] = ((const u8 *)r->kbuf)[i];
        devproc_apply_regs(target, r->kind, scratch);
    } else {
        for (long i = 0; i < cnt; i++) ((u8 *)r->kbuf)[i] = scratch[r->off + i];
    }
    r->result = cnt;
    return 1;
}

static long devproc_regs_rw(struct Spoor *c, void *buf, long n, s64 off, bool is_write) {
    if (!c || !buf || n < 0) return -1;
    if (off < 0)             return -1;
    if (n == 0)              return 0;
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    struct devproc_regs_ctx r = {
        .target_pid = proc_qid_pid(c->qid.path),
        .caller     = t->proc,
        .kind       = proc_qid_kind(c->qid.path),
        .kbuf       = buf,
        .n          = n,
        .off        = off,
        .is_write   = is_write,
        .result     = -1,
    };
    proc_for_each(devproc_regs_walk_cb, &r);
    return r.result;
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
    CTL_VERB_STEP,     // 8a-2b-2: single-step the head thread one instruction; block until re-stopped
    CTL_VERB_HWVERIFY, // 8a-2a: arm ("hwverify <hexva>") / disarm ("hwverify off") the self-scoped HW-breakpoint verify
    CTL_VERB_HWBREAK,  // 8a-2b-1: arm a real per-Proc HW breakpoint ("hwbreak <hexva>"; slot-owner + stopped-only)
    CTL_VERB_HWRMBREAK,// 8a-2b-1: disarm a per-Proc HW breakpoint ("hwrmbreak <hexva>")
    CTL_VERB_HWWATCH,  // 8a-2b-3: arm a per-Proc HW watchpoint ("hwwatch <rwx> <hexva> <declen>")
    CTL_VERB_HWRMWATCH,// 8a-2b-3: disarm a per-Proc HW watchpoint ("hwrmwatch <hexva>")
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
    if (ctl_tok_eq(s + start, len, "step"))     return CTL_VERB_STEP;
    if (ctl_tok_eq(s + start, len, "hwverify")) return CTL_VERB_HWVERIFY;
    if (ctl_tok_eq(s + start, len, "hwbreak"))  return CTL_VERB_HWBREAK;
    if (ctl_tok_eq(s + start, len, "hwrmbreak")) return CTL_VERB_HWRMBREAK;
    if (ctl_tok_eq(s + start, len, "hwwatch"))  return CTL_VERB_HWWATCH;
    if (ctl_tok_eq(s + start, len, "hwrmwatch")) return CTL_VERB_HWRMWATCH;
    return CTL_VERB_OTHER;
}

// 8a-2a: parse the hwverify argument -- the token after "hwverify". Returns:
//   HWVERIFY_ARG_OFF  = "off"    -> disarm
//   HWVERIFY_ARG_VA   = "0x..."  -> arm at *va_out (hex, user-half; caller bounds)
//   HWVERIFY_ARG_BAD  = missing / malformed
// The VA parse is a bounded hex scan of the second whitespace-delimited token;
// no libc, no allocation.
enum hwverify_arg { HWVERIFY_ARG_BAD, HWVERIFY_ARG_OFF, HWVERIFY_ARG_VA };
static enum hwverify_arg parse_hwverify_arg(const char *s, long n, u64 *va_out) {
    if (!s || n <= 0) return HWVERIFY_ARG_BAD;
    long i = 0;
    // skip the verb token
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (i < n && s[i] != ' '  && s[i] != '\t' && s[i] != '\n' &&
                    s[i] != '\r' && s[i] != '\0') i++;
    // skip the separator
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    long start = i;
    while (i < n && s[i] != ' '  && s[i] != '\t' && s[i] != '\n' &&
                    s[i] != '\r' && s[i] != '\0') i++;
    long len = i - start;
    if (len == 0) return HWVERIFY_ARG_BAD;
    if (ctl_tok_eq(s + start, len, "off")) return HWVERIFY_ARG_OFF;
    // hex VA, optionally 0x-prefixed
    const char *p = s + start;
    long j = 0;
    if (len >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) j = 2;
    if (j >= len) return HWVERIFY_ARG_BAD;   // "0x" with no digits
    u64 v = 0;
    for (; j < len; j++) {
        char c = p[j];
        u64 d;
        if (c >= '0' && c <= '9') d = (u64)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u64)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u64)(c - 'A' + 10);
        else return HWVERIFY_ARG_BAD;
        if (v > (~0ull >> 4)) return HWVERIFY_ARG_BAD;   // overflow guard
        v = (v << 4) | d;
    }
    *va_out = v;
    return HWVERIFY_ARG_VA;
}

// 8a-2b: parse the bare hex VA of a "hwbreak <hexva>" / "hwrmbreak <hexva>" write.
// Delegates to the hwverify scan (verb-skip + separator + optional-0x hex +
// overflow guard); a numeric token yields the VA, and "off" (not a VA) is
// correctly rejected. Returns true + *va_out on success.
static bool parse_ctl_hexva(const char *s, long n, u64 *va_out) {
    u64 v = 0;
    if (parse_hwverify_arg(s, n, &v) != HWVERIFY_ARG_VA) return false;
    *va_out = v;
    return true;
}

// 8a-2b-3: parse "hwwatch <rwx> <hexaddr> <declen>". *flags = DEBUG_WP_R|W from the
// rwx token ('r'/'w' chars, at least one), *addr = the hex VA (optional 0x), *len =
// the decimal region length 1..8. Returns false on any missing/malformed token.
// Self-contained bounded scan (no libc, no allocation); mirrors parse_hwverify_arg.
static bool parse_ctl_hwwatch(const char *s, long n, u8 *flags, u64 *addr, u32 *len) {
    if (!s || n <= 0) return false;
    long i = 0;
    long ts[4], tl[4];   // token [start,len) for verb + 3 args
    for (int k = 0; k < 4; k++) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        ts[k] = i;
        while (i < n && s[i] != ' '  && s[i] != '\t' && s[i] != '\n' &&
                        s[i] != '\r' && s[i] != '\0') i++;
        tl[k] = i - ts[k];
    }
    if (tl[1] == 0 || tl[2] == 0 || tl[3] == 0) return false;   // tl[0] = the verb (already matched)

    // rwx (token 1): 'r'/'w' (case-insensitive); at least one, no other char.
    u8 f = 0;
    for (long k = 0; k < tl[1]; k++) {
        char c = s[ts[1] + k];
        if (c == 'r' || c == 'R') f |= DEBUG_WP_R;
        else if (c == 'w' || c == 'W') f |= DEBUG_WP_W;
        else return false;
    }
    if (f == 0) return false;

    // hexaddr (token 2): optional 0x prefix, then hex digits, overflow-guarded.
    const char *p = s + ts[2];
    long pl = tl[2], j = 0;
    if (pl >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) j = 2;
    if (j >= pl) return false;   // "0x" with no digits
    u64 a = 0;
    for (; j < pl; j++) {
        char c = p[j];
        u64 d;
        if (c >= '0' && c <= '9') d = (u64)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u64)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u64)(c - 'A' + 10);
        else return false;
        if (a > (~0ull >> 4)) return false;   // overflow guard
        a = (a << 4) | d;
    }

    // declen (token 3): 1..8 decimal.
    const char *q = s + ts[3];
    long ql = tl[3];
    u32 l = 0;
    for (long k = 0; k < ql; k++) {
        char c = q[k];
        if (c < '0' || c > '9') return false;
        l = l * 10u + (u32)(c - '0');
        if (l > 8u) return false;   // clamp early -- v1.0 supports 1..8 bytes
    }
    if (l == 0u) return false;

    *flags = f;
    *addr = a;
    *len = l;
    return true;
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
        // 8a-2b-1: disarm every breakpoint (bp_count=0) BEFORE the resume, so the
        // resumed threads reload an EMPTY table (their next ctx-switch-IN) and do
        // not re-trap on an orphaned bp. Safe whether the target is stopped (the
        // resume's switch-IN clears MDE) or running (a stale-fire is caught benign
        // by hwdebug_breakpoint_from_el0). The table stays allocated (freed at
        // proc_free) so the ctx-switch reader never derefs freed memory.
        hwdebug_bp_clear_all(target->debug_hw);
        hwdebug_wp_clear_all(target->debug_hw);    // 8a-2b-3: likewise the watchpoints
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

// =============================================================================
// ctl HW breakpoints -- hwbreak / hwrmbreak (8a-2b-1; I-39, DEBUG-FS section 5).
// =============================================================================
//
// Arm/disarm a real per-Proc hardware breakpoint at a user VA. Gated like the
// run-control verbs (the caller's ctl fd must OWN the attach slot) PLUS the I-39
// two-axis re-check (a hardware-arming write) PLUS stopped-only (the target's
// threads are all parked -> no CPU is running a target thread -> the per-Proc bp
// table is quiescent, so the ctx-switch install reader never races the mutation;
// and Delve arms breakpoints on a stopped process). The lazy debug_hw table is
// pre-allocated by the caller OUTSIDE g_proc_table_lock (kzalloc may not run
// under the spinlock) and installed here if the target has none yet.
struct devproc_hwbp_ctx {
    int              target_pid;
    struct Proc     *caller;    // I-39 re-check
    struct Spoor    *ctl;       // must == target->debug_owner (slot ownership)
    bool             add;       // true = hwbreak, false = hwrmbreak
    u64              va;
    struct debug_hw *spare;     // pre-allocated table (add only); NULLed if installed
    int              result;    // 0 not found, +1 ok, -1 denied / full / not-present / not-stopped
};
static int devproc_hwbp_walk_cb(struct Proc *target, void *arg) {
    struct devproc_hwbp_ctx *h = (struct devproc_hwbp_ctx *)arg;
    if (target->pid != h->target_pid) return 0;                                    // keep walking
    if (target == kproc())                            { h->result = -1; return 1; } // undebuggable
    if (!devproc_debug_authorized(h->caller, target)) { h->result = -1; return 1; } // I-39
    if (target->debug_owner != h->ctl)                { h->result = -1; return 1; } // slot owner
    if (!devproc_target_fully_stopped(target))        { h->result = -1; return 1; } // stopped-only (quiescent)

    if (h->add) {
        struct debug_hw *hw = target->debug_hw;
        if (!hw) {
            hw = h->spare;
            if (!hw) { h->result = -1; return 1; }   // pre-alloc failed -> ENOMEM-ish
            h->spare = NULL;                         // consumed -- caller must not free it
            __atomic_store_n(&target->debug_hw, hw, __ATOMIC_RELEASE);
        }
        h->result = hwdebug_bp_add(hw, h->va) ? 1 : -1;   // -1 = table full or already armed
    } else {
        struct debug_hw *hw = target->debug_hw;
        h->result = (hw && hwdebug_bp_remove(hw, h->va)) ? 1 : -1;   // -1 = not present
    }
    return 1;
}

// =============================================================================
// ctl HW watchpoints -- hwwatch / hwrmwatch (8a-2b-3; I-39, DEBUG-FS section 5).
// =============================================================================
//
// The watchpoint twin of the hwbreak/hwrmbreak path: same gates (slot owner + the
// I-39 re-check + stopped-only so the per-Proc wp table is quiescent against the
// ctx-switch reader) + the same lazy debug_hw pre-alloc discipline (kzalloc OUTSIDE
// g_proc_table_lock; the spare is installed here iff the target has no table yet).
struct devproc_hwwatch_ctx {
    int              target_pid;
    struct Proc     *caller;    // I-39 re-check
    struct Spoor    *ctl;       // must == target->debug_owner (slot ownership)
    bool             add;       // true = hwwatch, false = hwrmwatch
    u64              va;
    u32              len;        // 1..8 (add only)
    u8               flags;      // DEBUG_WP_R|W (add only)
    struct debug_hw *spare;     // pre-allocated table (add only); NULLed if installed
    int              result;    // 0 not found, +1 ok, -1 denied / full / bad / not-present / not-stopped
};
static int devproc_hwwatch_walk_cb(struct Proc *target, void *arg) {
    struct devproc_hwwatch_ctx *h = (struct devproc_hwwatch_ctx *)arg;
    if (target->pid != h->target_pid) return 0;                                    // keep walking
    if (target == kproc())                            { h->result = -1; return 1; } // undebuggable
    if (!devproc_debug_authorized(h->caller, target)) { h->result = -1; return 1; } // I-39
    if (target->debug_owner != h->ctl)                { h->result = -1; return 1; } // slot owner
    if (!devproc_target_fully_stopped(target))        { h->result = -1; return 1; } // stopped-only (quiescent)

    if (h->add) {
        struct debug_hw *hw = target->debug_hw;
        if (!hw) {
            hw = h->spare;
            if (!hw) { h->result = -1; return 1; }   // pre-alloc failed -> ENOMEM-ish
            h->spare = NULL;                         // consumed -- caller must not free it
            __atomic_store_n(&target->debug_hw, hw, __ATOMIC_RELEASE);
        }
        h->result = hwdebug_wp_add(hw, h->va, h->len, h->flags) ? 1 : -1;   // -1 = full / dup / bad region
    } else {
        struct debug_hw *hw = target->debug_hw;
        h->result = (hw && hwdebug_wp_remove(hw, h->va)) ? 1 : -1;   // -1 = not present
    }
    return 1;
}

// =============================================================================
// ctl single-step -- step (8a-2b-2; I-39, DEBUG-FS section 5.5).
// =============================================================================
//
// Arm the arm64 SS machine on the target's HEAD thread and resume for exactly ONE
// EL0 instruction. Gated like the run-control verbs (slot owner) + the I-39
// re-check + stopped-only (the target's threads are parked, so the head's
// trapframe is settled -- the PC we read for the step-over decision is real). The
// step-over: if the head's PC is at an armed breakpoint, that bp is skipped during
// the step (else the resume re-traps on it instead of stepping). v1.0 resumes the
// WHOLE Proc (proc_debug_resume clears the per-Proc stop flag), so a multi-thread
// target's peers briefly run during the head's step -- the per-thread step is a
// v1.x refinement (with the per-thread /proc/<pid>/thread/<tid>/ layer). The EC
// 0x32 handler (hwdebug_singlestep_from_el0) re-stops the Proc after the one
// instruction; the caller then blocks until fully-stopped.
struct devproc_step_ctx {
    int           target_pid;
    struct Proc  *caller;   // I-39
    struct Spoor *ctl;      // must == target->debug_owner (slot ownership)
    int           result;   // 0 not found, +1 armed+resumed, -1 denied / not-stopped / no-thread
};
static int devproc_step_walk_cb(struct Proc *target, void *arg) {
    struct devproc_step_ctx *s = (struct devproc_step_ctx *)arg;
    if (target->pid != s->target_pid) return 0;                                    // keep walking
    if (target == kproc())                            { s->result = -1; return 1; } // undebuggable
    if (!devproc_debug_authorized(s->caller, target)) { s->result = -1; return 1; } // I-39
    if (target->debug_owner != s->ctl)                { s->result = -1; return 1; } // slot owner
    if (!devproc_target_fully_stopped(target))        { s->result = -1; return 1; } // stopped-only
    struct Thread *head = devproc_focus_thread(target);   // 8c-2 #95: the M at the stop (step targets it), else head
    if (!head)                                        { s->result = -1; return 1; } // no thread to step

    // The step-over VA: if the head's PC (its trapframe's ELR) sits at an armed
    // breakpoint, mark that bp to be skipped during the step (hwdebug_switch_in
    // loads it disabled for this thread). Read the trapframe -- valid because the
    // target is fully-stopped (the head is parked on this same entry).
    u64 pc = (head->debug_trapframe) ? (head->debug_trapframe->elr & ~3ull) : 0;
    u64 stepover = 0;
    struct debug_hw *hw = target->debug_hw;
    if (hw && pc) {
        u32 count = hw->bp_count;
        for (u32 i = 0; i < count && i < DEBUG_HWBP_SLOTS; i++)
            if (hw->bp_va[i] == pc) { stepover = pc; break; }
    }
    head->debug_stepover_va = stepover;
    __atomic_store_n(&head->debug_ss_armed, true, __ATOMIC_RELEASE);

    // Resume: clear debug_stop_req + wake the parked threads. The head runs one
    // instruction with SS armed (SPSR.SS set by el0_return_stop_check, MDSCR.SS by
    // hwdebug_switch_in), takes the EC 0x32, and the whole Proc re-stops.
    proc_debug_resume(target);
    s->result = 1;
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

// =============================================================================
// 8a-1b-gamma-3: /proc/<pid>/kstack -- the symbolized KERNEL backtrace (I-39;
// DEBUG-FS 4.6, the 8a-1 half of the unified user->kernel stack). The USER
// frames are the debugger's job (walk the EL0 x29 chain via /proc/<pid>/regs +
// /proc/<pid>/mem); the KERNEL frames need a kernel walker because the debugger
// cannot read kernel VAs (TTBR1) through mem. The walk reuses the audited Halls
// fp-chain logic via an ADDITIVE, fault-safe primitive (halls_walk_kernel_frames)
// that leaves the dying-machine dump path byte-unchanged.
//
// 8b (DEBUG-FS 5b): this read is the SETTLED-THREAD INSPECT tier -- the Linux
// /proc/<pid>/stack + Plan 9 model. Gate = I-39 authorization ONLY; NO
// debug-stop required (unlike mem/regs/kregs/wait, which stay fully_stopped-
// gated). The head thread is walked whenever it is SETTLED (on_cpu==false --
// parked on ANY rendez: the pipe/torpor/debug rendez); a running head reports
// "<running>". Memory-safe regardless of the target's concurrent execution
// (bounded to the thread's own kstack + the g_proc_table_lock lifetime pin);
// best-effort-consistent (coherent for a sleeping thread -- the "why is it
// hung" diagnostic -- garbage-but-bounded for a racing one). This is why 8b
// can show a thread blocked DEEP in a syscall (sleep -> the rendez), which a
// debug-stop -- parking only at the EL0-return tail -- cannot.
// =============================================================================

#define DEBUG_KSTACK_MAX_FRAMES  32u
#define DEBUG_KSTACK_BUF         2048

// Walk + symbolize the head thread's kernel stack into `buf`; return the byte
// length (0 if no walkable thread). Runs under g_proc_table_lock in the cb; the
// walk reads only the target's own USABLE kstack (guard excluded) + the .rodata
// symtab -- no sleep, no alloc. Truncates at a whole-line boundary if `cap` fills
// (best-effort; the shallow checkpoint stack fits easily -- a deep 8a-2 bp stack
// past 2 KiB is a v1.x offset-aware multi-read).
//
// 8b: th->ctx is canonical ONLY off-cpu (the #788 discipline: a running/switching
// thread's live state is in hardware, not th->ctx). Since the 8b relaxation drops
// the fully_stopped gate, a running head is possible -> report "<running>" (no
// walk of a stale ctx). An off-cpu head is walked as a settled snapshot; if it
// then wakes + runs mid-walk, halls_walk_kernel_frames keeps every deref inside
// [lo,hi) (memory-safe, garbage-but-bounded), the best-effort-consistency contract.
// `raw` -- may the caller see the raw slid kernel addresses? The raw `addr` + the
// unslid `link` reveal the KASLR slide (koff = addr - link), an I-16 secret that
// /ctl/kernel-base gates behind the CAP_HOSTOWNER tier (#57a). So they are emitted
// ONLY to a CAP_DEBUG/CAP_HOSTOWNER caller (the debugger tier -- it reads
// /ctl/kernel-base anyway + needs raw addrs to correlate with the kernel DWARF at
// 8c). The owner axis gets the KASLR-INDEPENDENT symbolic frame (name+soff is
// link-relative -> reveals no slide), which IS the "why is it hung" diagnostic.
// (8b-1d holotype F1: without this, an unprivileged owner reads its own koff off a
// settled head thread -- 8b widened the pre-existing 8a owner-attach-and-stop path.)
static size_t devproc_format_kstack(struct Proc *target, char *buf, size_t cap, bool raw) {
    struct Thread *th = devproc_focus_thread(target);   // 8c-2 #95: the M at the stop (else head)
    // 8a-1c holotype F1 (defense-in-depth): never walk an EXITING head thread's
    // ctx/kstack -- it is executing its exit path on that stack.
    if (!th || th->state == THREAD_EXITING || !th->kstack_base) return 0;
    if (th->kstack_size <= THREAD_KSTACK_GUARD_SIZE) return 0;   // no usable region
    // 8b: a running head has no meaningful saved frame (live regs are in HW).
    if (__atomic_load_n(&th->on_cpu, __ATOMIC_ACQUIRE))
        return fmt_str(buf, cap, 0, "#0  <running>\n");   // 0 if it did not fit

    // Fault-safe bounds: the USABLE kstack [base + guard, base + total). The low
    // guard is no-access -- walking into it would fault (there is no HX-I1 guard
    // on this LIVE path). halls_walk_kernel_frames additionally gates fp+16 <= hi.
    u64 base = (u64)(uintptr_t)th->kstack_base;
    u64 lo   = base + (u64)THREAD_KSTACK_GUARD_SIZE;
    u64 hi   = base + (u64)th->kstack_size;

    u64 frames[DEBUG_KSTACK_MAX_FRAMES];
    unsigned nf = halls_walk_kernel_frames(th->ctx.lr, th->ctx.fp, lo, hi,
                                           frames, DEBUG_KSTACK_MAX_FRAMES);
    u64 koff = kaslr_get_offset();

    size_t off = 0;
    for (unsigned i = 0; i < nf; i++) {
        size_t ls = off;   // line start -- truncate cleanly at a whole-line boundary
        u64 addr = frames[i];
        u64 link = halls_link_addr(addr, koff);
        u64 soff = 0;
        const char *name = halls_symbolize(link, &soff);   // .rodata table; NULL if unresolved
        size_t r;
        r = fmt_str(buf, cap, off, "#");        if (!r) return ls; off += r;
        r = fmt_udec(buf, cap, off, i);         if (!r) return ls; off += r;
        if (raw) {
            // I-16: the raw slid addr + unslid link leak koff -> CAP tier only.
            r = fmt_str(buf, cap, off, "  ");       if (!r) return ls; off += r;
            r = fmt_hex(buf, cap, off, addr);       if (!r) return ls; off += r;
            r = fmt_str(buf, cap, off, "  link ");  if (!r) return ls; off += r;
            r = fmt_hex(buf, cap, off, link);       if (!r) return ls; off += r;
        }
        if (name) {
            // name+soff is link-relative -> KASLR-independent (safe for the owner).
            r = fmt_str(buf, cap, off, "  ");    if (!r) return ls; off += r;
            r = fmt_str(buf, cap, off, name);    if (!r) return ls; off += r;
            r = fmt_str(buf, cap, off, "+");     if (!r) return ls; off += r;
            r = fmt_hex(buf, cap, off, soff);    if (!r) return ls; off += r;
        } else if (!raw) {
            // Owner axis + unsymbolized: emit no address (a bare slid addr leaks
            // koff). A CAP reader already got the raw addr above.
            r = fmt_str(buf, cap, off, "  <unknown>"); if (!r) return ls; off += r;
        }
        r = fmt_str(buf, cap, off, "\n");        if (!r) return ls; off += r;
    }
    return off;
}

struct devproc_kstack_ctx {
    int          target_pid;
    struct Proc *caller;
    char        *buf;
    size_t       cap;
    size_t       total;
    int          result;   // 0 not found, +1 built, -1 denied / no-thread
};

static int devproc_kstack_walk_cb(struct Proc *target, void *arg) {
    struct devproc_kstack_ctx *k = (struct devproc_kstack_ctx *)arg;
    if (target->pid != k->target_pid) return 0;   // keep walking
    if (target == kproc())                            { k->result = -1; return 1; }
    if (!devproc_debug_authorized(k->caller, target)) { k->result = -1; return 1; }   // I-39
    // 8b: the SETTLED-thread inspect -- NO debug-stop required (unlike mem/regs/
    // kregs/wait, which keep the fully_stopped gate). devproc_format_kstack gates
    // the head on on_cpu==false; the walk is bounded to the thread's own kstack
    // (memory-safe) + runs under g_proc_table_lock (the Thread lifetime pin). This
    // is what lets 8b inspect a thread blocked DEEP in a syscall (sleep -> rendez).
    //
    // I-16 (8b-1d holotype F1): raw slid kernel addresses reveal the KASLR slide, so
    // they go ONLY to the CAP_DEBUG/CAP_HOSTOWNER tier (the /ctl/kernel-base tier);
    // the owner axis gets the KASLR-independent symbolic form. Same acquire-load of
    // caps as devproc_debug_authorized (both axes are clearance-grantable).
    bool raw = (__atomic_load_n(&k->caller->caps, __ATOMIC_ACQUIRE)
                & (CAP_HOSTOWNER | CAP_DEBUG)) != 0;
    k->total  = devproc_format_kstack(target, k->buf, k->cap, raw);
    k->result = 1;
    return 1;
}

static long devproc_kstack_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf || n < 0) return -1;
    if (off < 0)             return -1;
    if (n == 0)              return 0;
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    char content[DEBUG_KSTACK_BUF];
    struct devproc_kstack_ctx k = {
        .target_pid = proc_qid_pid(c->qid.path),
        .caller     = t->proc,
        .buf        = content,
        .cap        = sizeof(content),
        .total      = 0,
        .result     = 0,
    };
    proc_for_each(devproc_kstack_walk_cb, &k);
    if (k.result != 1) return -1;   // not found / denied / not-stopped

    size_t total = k.total;
    if ((size_t)off >= total) return 0;   // EOF
    size_t avail = total - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;
    u8 *o = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) o[i] = (u8)content[(size_t)off + i];
    return (long)copy;
}

// =============================================================================
// 8a-1b-gamma-3: /proc/<pid>/wait -- the debugger's stop-notification channel.
// A read BLOCKS until the target is fully stopped (a checkpoint/trap park),
// exits, or the CALLER is death-interrupted; then returns a short status line.
// Gated by I-39 (the caller can debug the target) -- NOT slot ownership, since
// the wait fd is a distinct Spoor from the attach ctl fd (any authorized
// debugger may wait; the stop is driven by whoever owns the slot). Reuses the
// beta bounded re-poll -- lifetime-safe (no target pointer held across the lock
// drop; a reaped target is simply not found). The edge-triggered one-message-
// per-stop refinement is v1.x; v1.0 is level-triggered (already-stopped returns
// at once -- which still satisfies "blocks until the target stops").
// =============================================================================

struct devproc_waitscan_ctx {
    int          target_pid;
    struct Proc *caller;
    int          state;   // -2 denied (I-39/kproc), -1 gone/exited, 0 not-yet, 1 stopped
};
static int devproc_waitscan_cb(struct Proc *target, void *arg) {
    struct devproc_waitscan_ctx *w = (struct devproc_waitscan_ctx *)arg;
    if (target->pid != w->target_pid) return 0;   // keep walking -> not found -> stays -1
    if (target == kproc())                            { w->state = -2; return 1; }  // undebuggable
    if (!devproc_debug_authorized(w->caller, target)) { w->state = -2; return 1; }  // I-39
    if (target->state != PROC_STATE_ALIVE)            { w->state = -1; return 1; }  // exiting/zombie -> "exited"
    w->state = devproc_target_fully_stopped(target) ? 1 : 0;   // debug_stop_req + all parked
    return 1;
}

// Block until a wait event. Returns +1 stopped, 0 exited/gone, -1 denied /
// caller death-interrupted. Same bounded-poll cadence as devproc_debug_wait_stopped.
static int devproc_wait_block(int pid, struct Proc *caller) {
    struct Rendez pollr = RENDEZ_INIT;   // caller-private, single-waiter
    for (;;) {
        struct devproc_waitscan_ctx w = { .target_pid = pid, .caller = caller, .state = -1 };
        proc_for_each(devproc_waitscan_cb, &w);
        if (w.state == 1)  return 1;    // stopped
        if (w.state == -1) return 0;    // exited/gone
        if (w.state == -2) return -1;   // denied (I-39 / kproc)
        // state 0: ALIVE, not yet stopped -- sleep ~2 ms, then re-scan.
        u64 deadline = timer_now_ns() + DEBUG_STOP_POLL_NS;
        if (tsleep(&pollr, devproc_debug_poll_never, NULL, deadline) == TSLEEP_INTR)
            return -1;   // caller death-interrupted
    }
}

static long devproc_wait_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf || n < 0) return -1;
    if (off < 0)             return -1;
    if (n == 0)              return 0;
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    int ev = devproc_wait_block(proc_qid_pid(c->qid.path), t->proc);
    if (ev < 0) return -1;   // denied or caller death-interrupted
    const char *msg = ev ? "stopped\n" : "exited\n";

    size_t total = 0; while (msg[total]) total++;
    if ((size_t)off >= total) return 0;   // EOF
    size_t avail = total - (size_t)off;
    size_t copy = avail > (size_t)n ? (size_t)n : avail;
    u8 *o = (u8 *)buf;
    for (size_t i = 0; i < copy; i++) o[i] = (u8)msg[(size_t)off + i];
    return (long)copy;
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
    // regs/fpregs: `off` is the byte offset into the register struct.
    if (kind == PQS_REGS || kind == PQS_FPREGS) return devproc_regs_rw(c, (void *)buf, n, off, true);
    (void)off;
    if (kind != PQS_CTL) return -1;

    enum ctl_verb v = parse_ctl_verb((const char *)buf, n);
    if (v != CTL_VERB_KILL && v != CTL_VERB_KILLGRP &&
        v != CTL_VERB_ATTACH && v != CTL_VERB_DETACH &&
        v != CTL_VERB_STOP && v != CTL_VERB_START && v != CTL_VERB_WAITSTOP &&
        v != CTL_VERB_STEP && v != CTL_VERB_HWVERIFY &&
        v != CTL_VERB_HWBREAK && v != CTL_VERB_HWRMBREAK &&
        v != CTL_VERB_HWWATCH && v != CTL_VERB_HWRMWATCH) return -1;

    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;

    if (v == CTL_VERB_HWVERIFY) {
        // 8a-2c F2: the verify is a BOOT-ONLY diagnostic. The boot probe
        // usr/hwbp-verify runs before SYS_BOOT_COMPLETE (arm -> trigger -> read ->
        // disarm, all during boot); post-boot the real HW-debug path is the 8a-2b
        // per-Proc install, so the verify is vestigial. Refuse it post-boot: the
        // verify uses a GLOBAL one-at-a-time slot whose EC-swallow is keyed on an
        // ELR match, NOT the arming Proc, so an unprivileged Proc arming a lingering
        // verify could swallow another (authorized-debug) Proc's real breakpoint at
        // the same VA + transiently clear its MDE. Gating to the boot window removes
        // that surface entirely (no post-boot arm -> the global slot stays idle).
        if (boot_is_complete()) return -1;
        // 8a-2a: the self-scoped EL0 hardware-breakpoint verify. SELF-ONLY (the
        // arm targets the CALLER's own EL0 execution, so the target pid must be
        // the caller) -- which also satisfies I-39 by construction (a Proc is
        // always the owner of itself). Cross-Proc HW breakpoints are 8a-2b (the
        // per-thread install via a stopped debuggee). "off" disarms; a hex VA
        // (user-half, 4-byte aligned) arms bp0. The delivery result is read back
        // from this same ctl file (devproc_read_cb PQS_CTL).
        int target = proc_qid_pid(c->qid.path);
        if (target != t->proc->pid) return -1;    // self-test only
        u64 va = 0;
        switch (parse_hwverify_arg((const char *)buf, n, &va)) {
        case HWVERIFY_ARG_OFF:
            hwdebug_verify_disarm();
            return n;
        case HWVERIFY_ARG_VA:
            if (va == 0 || va >= UACCESS_USER_VA_TOP) return -1;   // user-half only
            return hwdebug_verify_arm(t->proc->pid, va) ? n : -1;  // -1 = a verify already armed
        default:
            return -1;
        }
    }

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

    if (v == CTL_VERB_STEP) {
        // 8a-2b-2: single-step the head thread one instruction. Arm + resume under
        // the lock, then block (outside the lock) until the target re-stops (the
        // step completed) / exits / the slot is released; -1 only if the CALLER was
        // death-interrupted.
        int pid = proc_qid_pid(c->qid.path);
        struct devproc_step_ctx s = { .target_pid = pid, .caller = t->proc, .ctl = c, .result = 0 };
        proc_for_each(devproc_step_walk_cb, &s);
        if (s.result != 1) return -1;
        // Block until the step COMPLETES + the target RE-stops. Must poll
        // devproc_target_fully_stopped (which checks debug_stop_req, re-set by the
        // EC 0x32 handler) via devproc_wait_block -- NOT devproc_debug_wait_stopped
        // (all_threads_parked), which would return prematurely on the STALE parked
        // state: proc_debug_resume clears debug_stop_req + wakes the head, but the
        // head's rendez_blocked_on stays &debug_rendez (+ on_cpu==false) until it
        // resumes from sleep(), so all_threads_parked reads "still parked" before
        // the step even runs. fully_stopped's debug_stop_req==0 gate rejects that
        // window and waits for the real re-stop.
        return (devproc_wait_block(pid, t->proc) >= 0) ? n : -1;
    }

    if (v == CTL_VERB_HWBREAK || v == CTL_VERB_HWRMBREAK) {
        // 8a-2b-1: arm/disarm a real per-Proc hardware breakpoint. Parse the VA
        // (user-half, non-zero), pre-allocate the lazy table OUTSIDE the lock (for
        // hwbreak; kzalloc must not run under g_proc_table_lock), then resolve +
        // gate + mutate under proc_for_each. A spare table not consumed by the cb
        // (target already had one, or the gate failed) is freed here.
        u64 va = 0;
        if (!parse_ctl_hexva((const char *)buf, n, &va)) return -1;
        if (va == 0 || va >= UACCESS_USER_VA_TOP) return -1;   // user-half only
        bool add = (v == CTL_VERB_HWBREAK);
        struct debug_hw *spare = add ? (struct debug_hw *)kzalloc(sizeof(struct debug_hw), 0) : NULL;
        struct devproc_hwbp_ctx h = {
            .target_pid = proc_qid_pid(c->qid.path),
            .caller     = t->proc,
            .ctl        = c,
            .add        = add,
            .va         = va,
            .spare      = spare,
            .result     = 0,
        };
        proc_for_each(devproc_hwbp_walk_cb, &h);
        if (h.spare) kfree(h.spare);   // not installed (target had a table / gate failed)
        return (h.result == 1) ? n : -1;
    }

    if (v == CTL_VERB_HWWATCH || v == CTL_VERB_HWRMWATCH) {
        // 8a-2b-3: arm/disarm a real per-Proc hardware watchpoint. hwwatch parses
        // "<rwx> <hexva> <declen>"; hwrmwatch parses just the VA (keyed removal).
        // Same lazy-table pre-alloc-outside-the-lock discipline as hwbreak.
        bool add = (v == CTL_VERB_HWWATCH);
        u64 va = 0;
        u32 len = 0;
        u8 flags = 0;
        if (add) {
            if (!parse_ctl_hwwatch((const char *)buf, n, &flags, &va, &len)) return -1;
        } else {
            if (!parse_ctl_hexva((const char *)buf, n, &va)) return -1;
        }
        if (va == 0 || va >= UACCESS_USER_VA_TOP) return -1;   // user-half only
        struct debug_hw *spare = add ? (struct debug_hw *)kzalloc(sizeof(struct debug_hw), 0) : NULL;
        struct devproc_hwwatch_ctx h = {
            .target_pid = proc_qid_pid(c->qid.path),
            .caller     = t->proc,
            .ctl        = c,
            .add        = add,
            .va         = va,
            .len        = len,
            .flags      = flags,
            .spare      = spare,
            .result     = 0,
        };
        proc_for_each(devproc_hwwatch_walk_cb, &h);
        if (h.spare) kfree(h.spare);   // not installed (target had a table / gate failed)
        return (h.result == 1) ? n : -1;
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

    // 8a-1b: the debug files are POSITIONED -- /proc/<pid>/mem is VA-addressed and
    // /proc/<pid>/{regs,kregs,kstack,wait} are struct/content-offset-addressed
    // (Plan 9's seekable /proc/<pid>/mem). So devproc read/write honor `off`, and
    // seekable must be set for SYS_PREAD/PWRITE/LSEEK to reach them (the #37 ESPIPE
    // gate rejects positioned I/O on a non-seekable Dev BEFORE devproc_read runs).
    // The pre-debug files (status/cmdline/ns) already slice at `off`; ctl is a
    // control channel whose verb parse ignores `off` (positioned writes are the
    // same kill/attach/stop authority -- no I-26/I-39 change).
    .seekable = true,

    .read     = devproc_read,
    .bread    = devproc_bread,
    .write    = devproc_write,
    .bwrite   = devproc_bwrite,

    .remove   = devproc_remove,
    .wstat    = devproc_wstat,
    .power    = devproc_power,
};
