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
//   /dev/consctl  — console mode control (LS-8b termios; the five line flags)
//   /dev/pts      — EMPTY synthetic mount-stub DIRECTORY (PTY-2a-2): joey
//                   MREPL-mounts ptyfs's devpts tree over it (the devhw
//                   /hw/pci precedent). Ungated visibility; no data node.
//
// dc='d'. The trivial leaves (null/zero/full/random/urandom) are world-rw and
// UNGATED -- the same on every Unix. cons/consctl are the console: `devdev_open`
// enforces the I-27 gate-at-namespace-open -- a console-attach check IDENTICAL to
// SYS_CONSOLE_OPEN's, so binding /dev/cons as a walkable path adds NO ungated
// front door to the single-reader console (see IDENTITY-DESIGN.md §9.8). cons
// ALSO re-gates its I/O; consctl's I/O is intentionally inheritance-reachable so
// a delegated login/shell can set the line discipline (#94-B -- the open-mint
// gate is the protection; see devdev_console_gate_ok).

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/poll.h>
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
    DEV_KIND_PTS     = 8,   // the /dev/pts mount-stub DIRECTORY (not a leaf)
    DEV_KIND_TAPESTRY = 9,  // the /dev/tapestry mount-stub DIRECTORY (G-3)
    DEV_KIND_CONSDRAIN = 10, // G-4: the renderer's console-output drain (RO)
    DEV_KIND_CONSFEED  = 11, // G-4: the renderer's input feed (WO)
    DEV_KIND_WINSIZE   = 12, // #55: the UNGATED read-only console-geometry leaf
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
    { "consdrain", DEV_KIND_CONSDRAIN },
    { "consfeed",  DEV_KIND_CONSFEED  },
    { "winsize",   DEV_KIND_WINSIZE   },   // #55: trivial-leaf class (ungated)
};

#define DEV_LEAF_COUNT  (sizeof(g_dev_leaves) / sizeof(g_dev_leaves[0]))

// cons + consctl are the console leaves. The OPEN gate (devdev_open) covers
// BOTH: opening either by NAME requires console-attach, so only the trusted
// console holder can MINT a cons/consctl fd through the namespace.
static bool dev_kind_is_console(u32 kind) {
    return kind == DEV_KIND_CONS || kind == DEV_KIND_CONSCTL;
}

// The console-DATA leaf. cons (NOT consctl) keeps the per-I/O re-gate: console
// INPUT is the single-reader resource whose theft is the A-5a-F2 break, so
// reads (and, symmetrically, writes/polls) of /dev/cons re-check console-attach
// even on an already-opened handle. consctl is the console-CONTROL leaf and is
// deliberately NOT in this set -- see devdev_console_gate_ok (#94-B).
static bool dev_kind_is_cons_io(u32 kind) {
    return kind == DEV_KIND_CONS;
}

// The I-27 console-attach gate, enforced at two tiers:
//
//   1. OPEN (both cons + consctl): only a console-attached caller can MINT a
//      cons/consctl fd by name. This is the mint gate.
//
//   2. cons I/O (read/write/poll of /dev/cons only): the data path re-checks
//      attach on every I/O, because SYS_WALK_OPEN_OPATH (T_OPATH) SKIPS
//      dev->open (kernel/syscall.c) and sys_read_for_proc gates only on
//      RIGHT_READ, so a gate-at-open-only design would let any EL0 Proc
//      O_PATH-open then read /dev/cons and steal the getty's console input.
//      (#81's CWALKONLY now also rejects an O_PATH-walked handle at the syscall
//      layer before dev->read; the cons I/O re-gate stays as belt-and-suspenders
//      on the single-reader input -- the highest-stakes leaf.)
//
// consctl I/O is intentionally NOT re-gated (#94-B): the console line discipline
// (termios) must be settable by a non-console-attached but deliberately-delegated
// Proc -- /sbin/login and the session shell set cooked/echo via an INHERITED
// consctl fd (the getty, console-attached, opens it pre-relinquish and passes it
// down). That is sound because (a) the open-mint gate means only a console-
// attached Proc creates a consctl fd; (b) #81's CWALKONLY blocks the O_PATH-walk
// bypass; (c) a consctl fd reaches a non-attached Proc ONLY by deliberate
// spawn-inheritance from the trusted chain. At #94-B-a the chain TERMINATES at
// login: joey opens consctl + hands it to login (child fd 3), but login's shell
// spawn carries only stdin/stdout/stderr (libthyla-rs Command's fd_list is the
// 3 stdio slots), so the fd does NOT reach ut yet. #94-B-b extends the chain to
// ut (via Command::inherit_fd), where ut holds it PRIVATELY (never passing it to
// a user child) for the raw/cooked dance. A random EL0 Proc never holds one. The
// inherited fd IS the capability. consctl is a CONTROL surface (the five mode
// flags); it can never read console INPUT (DEV_KIND_CONSCTL routes read ->
// cons_render_mode, NOT cons_input_read), so an ungated consctl write cannot
// exfiltrate a keystroke -- it only flips the global termios, which the trusted
// chain is exactly what's authorized to do. Fail-closed on a NULL thread/proc.
static bool devdev_console_gate_ok(void) {
    struct Thread *t = current_thread();
    return t != NULL && proc_is_console_attached(t->proc);
}

// G-4: the renderer leaves (the drain/feed pair, TAPESTRY.md section 18.12
// R2-F6 / F8). Gated at OPEN *and* re-gated at every I/O + poll on
// proc_is_console_renderer -- the third console role, distinct from BOTH
// attach (elevation) and owner (Ctrl-C). The I/O re-gate mirrors the cons
// data-leaf discipline: an O_PATH walk-open skips devdev_open (and #81
// CWALKONLY blocks its reads at the syscall layer), so the re-gate is
// belt-and-suspenders on the two highest-stakes new leaves -- the drain
// reads ALL console output (the F8 leak), the feed injects input into
// cooked/ECHO/ISIG. Fail-closed on a NULL thread/proc.
static bool dev_kind_is_renderer(u32 kind) {
    return kind == DEV_KIND_CONSDRAIN || kind == DEV_KIND_CONSFEED;
}

static bool devdev_renderer_gate_ok(void) {
    struct Thread *t = current_thread();
    return t != NULL && proc_is_console_renderer(t->proc);
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
        // The one DIRECTORY child: /dev/pts, the ptyfs mount stub (PTY-2a-2;
        // the devhw /hw/pci synth-child precedent). EMPTY pre-mount -- no child
        // resolves from it here; joey MREPL-mounts ptyfs's devpts tree over it,
        // and post-mount the resolver crosses into ptyfs BEFORE asking devdev,
        // so this qid serves only as the mount-point identity
        // (dc='d', devno, DEV_KIND_PTS).
        if (name_eq("pts", name)) {
            out_qid->path = (u64)DEV_KIND_PTS;
            out_qid->type = QTDIR;
            return true;
        }
        // /dev/tapestry: the compositor mount stub (Tapestry G-3, the V4
        // /dev/tapestry vote) -- same shape as pts: EMPTY pre-mount, joey
        // MREPL-mounts tapestryd's tree over it.
        if (name_eq("tapestry", name)) {
            out_qid->path = (u64)DEV_KIND_TAPESTRY;
            out_qid->type = QTDIR;
            return true;
        }
        for (size_t i = 0; i < DEV_LEAF_COUNT; i++) {
            if (name_eq(g_dev_leaves[i].name, name)) {
                out_qid->path = (u64)g_dev_leaves[i].kind;
                out_qid->type = QTFILE;
                return true;
            }
        }
        return false;
    }

    // From a leaf qid: walk has no meaning (leaves are files). The pts dir also
    // has no devdev children (empty by design -- ".." is handled above).
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
    if (dev_kind_is_console((u32)c->qid.path) && !devdev_console_gate_ok()) {
        // #55 (ARCH 23.5.3): consctl (the CONTROL leaf) is ALSO mintable by
        // the bound renderer -- the winsize writer self-serves by name,
        // exactly as it opens consdrain/consfeed. Sound ONLY because the
        // renderer-minted consctl is restricted to the `winsize` verb (the
        // CCONSWINSZONLY tag below): the renderer already holds consfeed =
        // arbitrary INPUT INJECTION, which dominates a winsize report (pure
        // geometry, no input-domain authority) -- but it does NOT dominate a
        // termios flip on the GLOBAL cooking word, which also governs the
        // SERIAL RX path (#55 audit F2). cons (the DATA leaf) stays
        // attach-only: reading console INPUT is exactly what the renderer
        // role must NOT confer (I-27; the drain carries OUTPUT).
        if (!((u32)c->qid.path == DEV_KIND_CONSCTL &&
              devdev_renderer_gate_ok()))
            return NULL;
        // The renderer-minted (non-attached) consctl: winsize-verb-only.
        struct Spoor *o = dev_simple_open(c, omode);
        if (o) o->flag |= CCONSWINSZONLY;
        return o;
    }
    // G-4: the renderer pair mints only for the bound renderer. The drain
    // open additionally ARMS the tap (single-open; a second open is refused
    // by cons_drain_open) -- and unwinds the arm if the generic open then
    // fails, so a failed mint never leaves a fidless armed drain.
    if (dev_kind_is_renderer((u32)c->qid.path)) {
        if (!devdev_renderer_gate_ok()) return NULL;
        if ((u32)c->qid.path == DEV_KIND_CONSDRAIN) {
            if (cons_drain_open() != 0) return NULL;
            struct Spoor *o = dev_simple_open(c, omode);
            if (!o) cons_drain_close();
            return o;
        }
    }
    return dev_simple_open(c, omode);
}

static struct Spoor *devdev_create(struct Spoor *c, const char *name, int omode, u32 perm, u32 gid) {
    (void)c; (void)name; (void)omode; (void)perm; (void)gid;
    return NULL;
}

static void devdev_close(struct Spoor *c) {
    // G-4: the OPENED drain Spoor's close disarms the tap. The COPEN check is
    // load-bearing: devdev_close fires for every clunked devdev Spoor,
    // including never-opened walk intermediates and O_PATH handles (which
    // skip dev->open and thus never armed) -- only the Spoor that actually
    // minted through devdev_open carries COPEN, and there is exactly one at
    // a time (cons_drain_open's single-open). Runs at the LAST handle ref
    // (dup/inherited fds share the one Spoor), incl. the renderer's
    // #926/#68 close-at-exit -- so a dead renderer always disarms.
    if (c && (u32)c->qid.path == DEV_KIND_CONSDRAIN && (c->flag & COPEN))
        cons_drain_close();
    dev_simple_close(c);
}

static long devdev_read(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    if (n < 0) return -1;

    u32 kind = (u32)c->qid.path;
    // I-27: the console-DATA I/O gate -- cons ONLY (also at open; re-checked here
    // to close the O_PATH bypass). consctl is NOT re-gated: its mode line is
    // readable by a deliberately-delegated holder of an inherited consctl fd
    // (#94-B -- see devdev_console_gate_ok). The open-mint gate + #81 CWALKONLY
    // are the protections; the inherited fd is the capability.
    if (dev_kind_is_cons_io(kind) && !devdev_console_gate_ok()) return -1;
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
    case DEV_KIND_CONSCTL: {                     // LS-8b: read back the mode line
        // #55: the render now ends "... winsize <cols> <rows>\n" (the ptyfs
        // ctl_render shape) -- max 34 + " 65535 65535"-class tail = 54.
        char tmp[64];
        long len = cons_render_mode(tmp, (long)sizeof(tmp));
        if (off < 0 || off >= len) return 0;     // EOF (and bad offset reads empty)
        long avail = len - (long)off;
        long cnt = (n < avail) ? n : avail;
        u8 *out = (u8 *)buf;
        for (long i = 0; i < cnt; i++) out[i] = (u8)tmp[(long)off + i];
        return cnt;
    }
    case DEV_KIND_WINSIZE: {                     // #55: `winsize <cols> <rows>\n`
        char tmp[24];                            // max 21 incl. NL
        long len = cons_render_winsize(tmp, (long)sizeof(tmp));
        if (off < 0 || off >= len) return 0;
        long avail = len - (long)off;
        long cnt = (n < avail) ? n : avail;
        u8 *out = (u8 *)buf;
        for (long i = 0; i < cnt; i++) out[i] = (u8)tmp[(long)off + i];
        return cnt;
    }
    case DEV_KIND_CONSDRAIN:                     // G-4: the renderer's stream
        if (!devdev_renderer_gate_ok()) return -1;
        return cons_drain_read(buf, n);
    case DEV_KIND_CONSFEED:                      // write-only
        return -1;
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
    // I-27: the console-DATA I/O gate -- cons ONLY (also at open; re-checked here
    // to close the O_PATH bypass). consctl writes (the termios mode-set) are NOT
    // re-gated: a deliberately-delegated holder of an inherited consctl fd
    // (login/ut) flips the line discipline (#94-B -- see devdev_console_gate_ok).
    if (dev_kind_is_cons_io(kind) && !devdev_console_gate_ok()) return -1;
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
    case DEV_KIND_CONSCTL:                      // LS-8b: stty-style +/-flag parse
        // #55 audit F2: a renderer-minted consctl (CCONSWINSZONLY) is
        // restricted to the winsize verb; the attached chain gets full flags.
        return cons_set_mode_cmd(buf, n, !(c->flag & CCONSWINSZONLY));
    case DEV_KIND_CONSFEED:                     // G-4: renderer input injection
        if (!devdev_renderer_gate_ok()) return -1;
        return cons_feed_write(buf, n);
    case DEV_KIND_CONSDRAIN:                    // read-only
    case DEV_KIND_WINSIZE:                      // #55: read-only (the writer is
        return -1;                              // the consctl verb, renderer-held)
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

// #55: SYS_FSTAT on the /dev/cons leaf -- the shared is-a-cons contract
// (cons_stat_native_fill; the same fill devcons reports, so the std-fd
// inheritance chain and a namespace-opened /dev/cons agree). Other leaves
// stay statless (-1) -- the contract is cons-scoped; widening it to the
// trivial leaves is an undesigned nicety, not a #55 dependency.
static int devdev_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    if ((u32)c->qid.path != DEV_KIND_CONS) return -1;
    return cons_stat_native_fill(c, out);
}

static int devdev_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devdev_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

// LS-8a: poll dispatch. The cons leaf delegates to the shared cons_poll (real RX
// readiness + the deferred-wake hook); the trivial leaves never block, so they
// are always ready for the requested events (the NULL-.poll semantics, made
// explicit here so /dev/cons can differ). The console leaves are I-27-gated here
// too (like read/write): an O_PATH walk-open skips devdev_open, so a
// non-attached caller could otherwise register a wake hook on the console and
// learn its input timing -- POLLNVAL closes it. consctl is modeless at v1.0 (the
// termios write surface is LS-8b) -> no ready events.
static short devdev_poll(struct Spoor *c, short events, struct poll_waiter *pw) {
    if (!c) return POLLNVAL;
    u32 kind = (u32)c->qid.path;
    // cons (the data path) stays I/O-gated -- an O_PATH walk-open skips
    // devdev_open, so a non-attached poller could otherwise register a wake hook
    // and learn console-input timing; POLLNVAL closes it.
    if (dev_kind_is_cons_io(kind)) {
        if (!devdev_console_gate_ok()) return POLLNVAL;
        return cons_poll(events, pw);
    }
    // G-4: the renderer pair is gated here too (the same O_PATH rationale --
    // a non-renderer poller must not register a hook that learns console
    // OUTPUT timing). The drain has real readiness; the feed never blocks.
    if (dev_kind_is_renderer(kind)) {
        if (!devdev_renderer_gate_ok()) return POLLNVAL;
        if (kind == DEV_KIND_CONSDRAIN) return cons_drain_poll(events, pw);
        return (short)(events & POLL_REQUESTABLE);
    }
    // consctl + the trivial leaves: never block -- always ready for the requested
    // events. consctl poll is UNGATED (#94-B): it returns a state-INDEPENDENT
    // constant (always-ready) and installs NO data-readiness hook, so it leaks
    // nothing and grants nothing even if reached via an O_PATH handle (whose
    // read/write CWALKONLY blocks at the syscall layer); a control surface has no
    // input timing to leak.
    return (short)(events & POLL_REQUESTABLE);
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
    .stat_native = devdev_stat_native,   // #55: cons-leaf is-a-cons contract

    .open     = devdev_open,
    .create   = devdev_create,
    .close    = devdev_close,

    .read     = devdev_read,
    .bread    = devdev_bread,
    .write    = devdev_write,
    .bwrite   = devdev_bwrite,
    .poll     = devdev_poll,

    .remove   = devdev_remove,
    .wstat    = devdev_wstat,
    .power    = devdev_power,
};
