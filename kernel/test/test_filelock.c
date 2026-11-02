// SYS_FILE_LOCK and the Proc owner table (B-3h; ARCHITECTURE.md 9.6.11).
//
// A fake Dev serves the four lock slots over a small table keyed by the owner
// Spoor (Stratum keys by fid, so a Spoor here plays a fid): a conflict is an
// overlap with another owner where either lock is a write lock. Each test's
// fixtures are opened by its outer function and released there, after the
// asserting body returns, so a failed assert leaks nothing into kproc's table.

#include "test.h"

#include "../../arch/arm64/exception.h"
#include "../../arch/arm64/uaccess.h"

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/filelock.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_filelock_refusals(void);
void test_filelock_file_owner(void);
void test_filelock_proc_owner(void);
void test_filelock_close_drops(void);
void test_filelock_wait(void);
void test_filelock_failed_take_spends_no_slot(void);
void test_filelock_bound(void);
void test_filelock_close_mid_call(void);
void test_filelock_proc_test_owner(void);
void test_filelock_fork_exec_dup_teardown(void);
void test_filelock_stale_entry_retired(void);
void test_filelock_reused_number(void);
void test_filelock_acquire_syncs(void);
void test_filelock_last_descriptor_closes(void);

// -----------------------------------------------------------------------------
// The fake Dev.
// -----------------------------------------------------------------------------

#define LK_MAXREC 192

struct lk_rec {
    struct Spoor *owner;
    u64           path;
    u32           type;
    u64           start, end;   // [start, end)
    bool          live;
};

static struct lk_rec  g_lk[LK_MAXREC];
static u32            g_lk_seq;            // bumps on every fake op
static u32            g_lk_calls;          // .lock calls
static u32            g_lk_agains;         // answer this many takes EAGAIN first
static u32            g_lk_mints;          // .lock_owner successes
static u32            g_lk_owner_closes;   // minted owners closed
static int            g_lk_mint_fail;      // != 0: .lock_owner fails with it
static struct Spoor  *g_lk_last_spoor;     // the Spoor the last .lock acted as
static u32            g_lk_last_type;
static u64            g_lk_last_start, g_lk_last_len;
static bool           g_lk_last_test;
static u32            g_lk_un_seq;         // seq of the last unlock
static u32            g_lk_sync_n;         // .lock_sync(RELEASE) calls
static u32            g_lk_sync_last_seq;
static struct Spoor  *g_lk_synced[8];
static u32            g_lk_acq_n;          // .lock_sync(ACQUIRE) calls
static u32            g_lk_acq_last_seq;
static struct Spoor  *g_lk_acquired[8];
static struct Spoor  *g_lk_acq_fail;       // .lock_sync(ACQUIRE) on this Spoor fails
static int            g_lk_acq_fail_rc;
static u32            g_lk_take_seq;       // seq of the last take that reached .lock
static hidx_t         g_lk_close_fd = -1;  // close this fd inside the next .lock
static u8             g_lk_owner_tag;      // aux of a minted owner
static struct Spoor  *g_lk_stale;          // .lock on this Spoor answers ESTALE
static u64            g_lk_stale_path;     // != 0: every .lock on this path does
static struct Spoor  *g_lk_dead;           // .lock_ident on this Spoor answers ESTALE
static int            g_lk_ident_fail;     // != 0: and answers this instead
static u32            g_lk_idents;         // .lock_ident calls that asked the "server"
static u32            g_lk_fd_closes;      // .fd_close calls
static u32            g_lk_held_close_seq; // seq of the last .fd_close that held a lock
static struct Spoor  *g_lk_fd_close_last;  // the Spoor it last ran on
static u32            g_lk_closes;         // .close calls: last references
// A fake Spoor's generation is its qid.vers once .lock_ident has learned it (0
// = not yet); the file's true one is registered here (default 1).
static struct { struct Spoor *s; u32 gen; } g_lk_gens[8];

static struct Dev g_lk_dev;

static void lk_reset(void) {
    for (u32 i = 0; i < LK_MAXREC; i++) g_lk[i].live = false;
    g_lk_seq = g_lk_calls = g_lk_agains = g_lk_mints = g_lk_owner_closes = 0;
    g_lk_mint_fail = 0;
    g_lk_last_spoor = NULL;
    g_lk_last_type = 0;
    g_lk_last_start = g_lk_last_len = 0;
    g_lk_last_test = false;
    g_lk_un_seq = g_lk_sync_n = g_lk_sync_last_seq = 0;
    for (u32 i = 0; i < 8; i++) g_lk_synced[i] = NULL;
    g_lk_acq_n = g_lk_acq_last_seq = g_lk_take_seq = 0;
    for (u32 i = 0; i < 8; i++) g_lk_acquired[i] = NULL;
    g_lk_acq_fail = NULL;
    g_lk_acq_fail_rc = 0;
    g_lk_close_fd = -1;
    g_lk_stale = NULL;
    g_lk_stale_path = 0;
    g_lk_dead = NULL;
    g_lk_ident_fail = 0;
    for (u32 i = 0; i < 8; i++) { g_lk_gens[i].s = NULL; g_lk_gens[i].gen = 0; }
    g_lk_idents = 0;
}

static void lk_set_gen(struct Spoor *s, u32 gen) {
    for (u32 i = 0; i < 8; i++)
        if (!g_lk_gens[i].s || g_lk_gens[i].s == s) {
            g_lk_gens[i].s = s;
            g_lk_gens[i].gen = gen;
            return;
        }
}

static u32 lk_true_gen(struct Spoor *s) {
    for (u32 i = 0; i < 8; i++)
        if (g_lk_gens[i].s == s) return g_lk_gens[i].gen;
    return 1;
}

static u64 lk_end(u64 start, u64 len) {
    if (len == 0 || start + len < start) return (u64)-1ll;
    return start + len;
}

static bool lk_overlap(u64 s1, u64 e1, u64 s2, u64 e2) { return s1 < e2 && s2 < e1; }

static void lk_drop_owner(struct Spoor *owner, u64 path, u64 s, u64 e) {
    for (u32 i = 0; i < LK_MAXREC; i++)
        if (g_lk[i].live && g_lk[i].owner == owner && g_lk[i].path == path &&
            lk_overlap(g_lk[i].start, g_lk[i].end, s, e))
            g_lk[i].live = false;
}

static u32 lk_held_on(u64 path) {
    u32 n = 0;
    for (u32 i = 0; i < LK_MAXREC; i++)
        if (g_lk[i].live && g_lk[i].path == path) n++;
    return n;
}

static int lk_lock(struct Spoor *c, u32 type, u64 start, u64 len,
                   struct t_file_lock *test) {
    g_lk_calls++;
    g_lk_seq++;
    if (!test && type != SYS_FILE_LOCK_UN) g_lk_take_seq = g_lk_seq;
    if (g_lk_close_fd >= 0) {       // a peer thread's close lands mid-call
        hidx_t fd = g_lk_close_fd;
        g_lk_close_fd = -1;
        handle_close(current_thread()->proc, fd);
    }
    // A fid of a file that has died (the server answers ESTALE; its records
    // went with the file).
    if (c == g_lk_stale || (g_lk_stale_path && c->qid.path == g_lk_stale_path))
        return -T_E_STALE;
    u64 end = lk_end(start, len);
    g_lk_last_spoor = c;
    g_lk_last_type  = type;
    g_lk_last_start = start;
    g_lk_last_len   = len;
    g_lk_last_test  = test != NULL;
    if (test) {
        for (u32 i = 0; i < LK_MAXREC; i++) {
            struct lk_rec *r = &g_lk[i];
            if (r->live && r->path == c->qid.path && r->owner != c &&
                lk_overlap(r->start, r->end, start, end) &&
                (r->type == SYS_FILE_LOCK_WR || type == SYS_FILE_LOCK_WR)) {
                test->type  = (u8)r->type;
                test->start = r->start;
                test->len   = r->end == (u64)-1ll ? 0 : r->end - r->start;
                test->pid   = -1;
                return 0;
            }
        }
        test->type  = SYS_FILE_LOCK_UN;
        test->start = start;
        test->len   = len;
        test->pid   = -1;
        return 0;
    }
    if (type == SYS_FILE_LOCK_UN) {
        g_lk_un_seq = g_lk_seq;
        lk_drop_owner(c, c->qid.path, start, end);
        return 0;
    }
    if (g_lk_agains) {
        g_lk_agains--;
        return -T_E_AGAIN;
    }
    for (u32 i = 0; i < LK_MAXREC; i++) {
        struct lk_rec *r = &g_lk[i];
        if (r->live && r->path == c->qid.path && r->owner != c &&
            lk_overlap(r->start, r->end, start, end) &&
            (r->type == SYS_FILE_LOCK_WR || type == SYS_FILE_LOCK_WR))
            return -T_E_AGAIN;
    }
    lk_drop_owner(c, c->qid.path, start, end);
    for (u32 i = 0; i < LK_MAXREC; i++) {
        if (g_lk[i].live) continue;
        g_lk[i] = (struct lk_rec){ c, c->qid.path, type, start, end, true };
        return 0;
    }
    return -T_E_NOLCK;
}

static struct Spoor *lk_lock_owner(struct Spoor *c, int *err) {
    if (g_lk_mint_fail) { *err = g_lk_mint_fail; return NULL; }
    struct Spoor *o = spoor_alloc(&g_lk_dev);
    if (!o) { *err = -T_E_NOLCK; return NULL; }
    o->qid = c->qid;
    o->aux = &g_lk_owner_tag;
    g_lk_mints++;
    return o;
}

// dev9p's rule: the number, and the generation when both have learned it.
static bool lk_same_file(struct Spoor *a, struct Spoor *b) {
    if (a->qid.path != b->qid.path) return false;
    return a->qid.vers == 0 || b->qid.vers == 0 || a->qid.vers == b->qid.vers;
}

static int lk_ident(struct Spoor *c) {
    if (c->qid.vers != 0) return 0;
    g_lk_idents++;
    if (c == g_lk_dead) return g_lk_ident_fail ? g_lk_ident_fail : -T_E_STALE;
    c->qid.vers = lk_true_gen(c);
    return 0;
}

static int lk_sync(struct Spoor *c, u32 how) {
    g_lk_seq++;
    if (how == DEV_LOCK_SYNC_ACQUIRE) {
        if (g_lk_acq_n < 8) g_lk_acquired[g_lk_acq_n] = c;
        g_lk_acq_n++;
        g_lk_acq_last_seq = g_lk_seq;
        return c == g_lk_acq_fail ? g_lk_acq_fail_rc : 0;
    }
    if (g_lk_sync_n < 8) g_lk_synced[g_lk_sync_n] = c;
    g_lk_sync_n++;
    g_lk_sync_last_seq = g_lk_seq;
    return 0;
}

static bool lk_held(struct Spoor *c) {
    for (u32 i = 0; i < LK_MAXREC; i++)
        if (g_lk[i].live && g_lk[i].owner == c) return true;
    return false;
}

// A last close releases what this Spoor holds, as dev9p's whole-file unlock does.
static int lk_fd_close(struct Spoor *c) {
    g_lk_seq++;
    g_lk_fd_closes++;
    g_lk_fd_close_last = c;
    if (lk_held(c)) g_lk_held_close_seq = g_lk_seq;
    for (u32 i = 0; i < LK_MAXREC; i++)
        if (g_lk[i].live && g_lk[i].owner == c) g_lk[i].live = false;
    return 0;
}

// A last clunk is the server releasing that fid's locks (Stratum drops a
// fid's records at Tclunk), so a freed Spoor's address never inherits them, nor
// the stale mark: the next owner minted can take that address.
static int lk_close(struct Spoor *c) {
    g_lk_closes++;
    if (c->aux == &g_lk_owner_tag) g_lk_owner_closes++;
    if (c == g_lk_stale) g_lk_stale = NULL;
    for (u32 i = 0; i < LK_MAXREC; i++)
        if (g_lk[i].live && g_lk[i].owner == c) g_lk[i].live = false;
    c->aux = NULL;
    return 0;
}

static struct Dev g_lk_dev = {
    .dc             = 'k',
    .name           = "locktest",
    .close          = lk_close,
    .fd_close       = lk_fd_close,
    .lock_held      = lk_held,
    .lock           = lk_lock,
    .lock_owner     = lk_lock_owner,
    .lock_same_file = lk_same_file,
    .lock_sync      = lk_sync,
    .lock_ident     = lk_ident,
};

static struct Dev g_lk_nolock_dev = {
    .dc   = 'K',
    .name = "locktest-none",
};

static bool lk_synced(struct Spoor *c) {
    for (u32 i = 0; i < 8 && i < g_lk_sync_n; i++)
        if (g_lk_synced[i] == c) return true;
    return false;
}

static bool lk_acquired(struct Spoor *c) {
    for (u32 i = 0; i < 8 && i < g_lk_acq_n; i++)
        if (g_lk_acquired[i] == c) return true;
    return false;
}

static void lk_sync_reset(void) {
    g_lk_sync_n = g_lk_sync_last_seq = 0;
    for (u32 i = 0; i < 8; i++) g_lk_synced[i] = NULL;
}

static void lk_acq_reset(void) {
    g_lk_acq_n = g_lk_acq_last_seq = g_lk_take_seq = 0;
    for (u32 i = 0; i < 8; i++) g_lk_acquired[i] = NULL;
}

// -----------------------------------------------------------------------------
// Fixtures.
// -----------------------------------------------------------------------------

#define LK_MAXFD (PROC_LOCK_OWNER_MAX + 8)

struct lk_fix {
    struct Proc  *p;
    hidx_t        fd[LK_MAXFD];
    struct Spoor *sp[LK_MAXFD];
    u32           n;
};

// Open a fake file (qid.path `path`) in f->p with `rights`; returns its index
// in the fixture, or -1.
static int lk_open(struct lk_fix *f, struct Dev *d, u64 path, rights_t rights) {
    if (f->n >= LK_MAXFD) return -1;
    struct Spoor *s = spoor_alloc(d);
    if (!s) return -1;
    s->qid.path = path;
    s->qid.type = QTFILE;
    hidx_t fd = handle_alloc(f->p, KOBJ_SPOOR, rights, s);
    if (fd < 0) { spoor_clunk(s); return -1; }
    f->fd[f->n] = fd;
    f->sp[f->n] = s;
    return (int)f->n++;
}

// Close fixture fd i (once).
static void lk_close_fd(struct lk_fix *f, int i) {
    if (i < 0 || f->fd[i] < 0) return;
    handle_close(f->p, f->fd[i]);
    f->fd[i] = -1;
}

// Release every fixture fd, then drop any lock-owner entry left on paths
// 1..maxpath (a close of a descriptor of the file drops it).
static void lk_teardown(struct lk_fix *f, u64 maxpath) {
    for (u32 i = 0; i < f->n; i++) lk_close_fd(f, (int)i);
    for (u64 path = 1; path <= maxpath && handle_lock_owner_count(f->p); path++) {
        struct Spoor *s = spoor_alloc(&g_lk_dev);
        if (!s) break;
        s->qid.path = path;
        hidx_t fd = handle_alloc(f->p, KOBJ_SPOOR, RIGHT_READ, s);
        if (fd < 0) { spoor_clunk(s); break; }
        handle_close(f->p, fd);
    }
    g_lk_close_fd = -1;
}

static s64 lk_call_out(struct lk_fix *f, int i, u64 op, u64 start, u64 len, u64 out) {
    return sys_file_lock_handler((u64)f->fd[i], op, start, len, out);
}

static s64 lk_call(struct lk_fix *f, int i, u64 op, u64 start, u64 len) {
    return lk_call_out(f, i, op, start, len, 0x10000000ull);
}

static void lk_fix_init(struct lk_fix *f) {
    lk_reset();
    f->p = current_thread()->proc;
    f->n = 0;
    for (u32 i = 0; i < LK_MAXFD; i++) { f->fd[i] = -1; f->sp[i] = NULL; }
}

#define RW (RIGHT_READ | RIGHT_WRITE)

// -----------------------------------------------------------------------------
// The op word, the rights, the descriptor.
// -----------------------------------------------------------------------------

static void refusals_body(struct lk_fix *f, int rw, int ro, int wo, int opath,
                          int nolock, hidx_t dead, hidx_t other_kind) {
    const u64 W = SYS_FILE_LOCK_WR, R = SYS_FILE_LOCK_RD, U = SYS_FILE_LOCK_UN;
    TEST_EXPECT_EQ(lk_call(f, rw, W | 0x80, 0, 0), -(s64)T_E_INVAL, "an unknown bit is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, W | 0x100, 0, 0), -(s64)T_E_INVAL, "a bit above the flags is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, 3, 0, 0), -(s64)T_E_INVAL, "type 3 is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, U | SYS_FILE_LOCK_TEST, 0, 0), -(s64)T_E_INVAL,
                   "a test of UN is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, W | SYS_FILE_LOCK_TEST | SYS_FILE_LOCK_WAIT, 0, 0),
                   -(s64)T_E_INVAL, "a test that waits is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, W, 2, (u64)-1ll), -(s64)T_E_INVAL,
                   "a range past 2^64 is EINVAL");
    TEST_EXPECT_EQ(lk_call(f, rw, W, 1, (u64)-1ll), -(s64)T_E_INVAL,
                   "a range reaching 2^64 is EINVAL");
    TEST_EXPECT_EQ(g_lk_calls, 0u, "no refusal reached the Dev");
    // The control one byte away: the range ends at exactly 2^64 - 1.
    TEST_EXPECT_EQ(lk_call(f, rw, W, 1, (u64)-2ll), (s64)0, "a range ending at 2^64 - 1 locks");
    TEST_EXPECT_EQ(lk_call(f, rw, U, 0, 0), (s64)0, "and unlocks");

    u32 calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, ro, W, 0, 1), -(s64)T_E_BADF, "a write lock on a read-only fd is EBADF");
    TEST_EXPECT_EQ(lk_call(f, wo, R, 0, 1), -(s64)T_E_BADF, "a read lock on a write-only fd is EBADF");
    TEST_EXPECT_EQ(lk_call(f, opath, U, 0, 0), -(s64)T_E_BADF, "an O_PATH fd is EBADF");
    TEST_EXPECT_EQ(sys_file_lock_handler((u64)dead, W, 0, 1, 0), -(s64)T_E_BADF,
                   "a closed fd is EBADF");
    TEST_EXPECT_EQ(sys_file_lock_handler((u64)other_kind, U, 0, 0, 0), -(s64)T_E_BADF,
                   "a handle of another kind is EBADF");
    TEST_EXPECT_EQ(sys_file_lock_handler((u64)1 << 20, U, 0, 0, 0), -(s64)T_E_BADF,
                   "an index past the table is EBADF");
    TEST_EXPECT_EQ(g_lk_calls, calls, "no EBADF reached the Dev");
    TEST_EXPECT_EQ(lk_call(f, nolock, U, 0, 0), -(s64)T_E_NOLCK,
                   "a Dev with no lock slots is ENOLCK");

    TEST_EXPECT_EQ(lk_call(f, ro, U, 0, 0), (s64)0, "an unlock needs no right");
    // An answer the copy-out could not recover from never reaches it, and the
    // question never reaches the server.
    const u64 T = R | SYS_FILE_LOCK_TEST;
    calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call_out(f, wo, T, 0, 1, 0xffff000000000000ull), -(s64)T_E_FAULT,
                   "a kernel-half answer is EFAULT");
    TEST_EXPECT_EQ(lk_call_out(f, wo, T, 0, 1, 0), -(s64)T_E_FAULT, "so is a null one");
    TEST_EXPECT_EQ(lk_call_out(f, wo, T, 0, 1, UACCESS_USER_VA_TOP - 8), -(s64)T_E_FAULT,
                   "one that runs past the user half");
    TEST_EXPECT_EQ(lk_call_out(f, wo, T, 0, 1, (u64)-8ll), -(s64)T_E_FAULT, "and one that wraps");
    TEST_EXPECT_EQ(g_lk_calls, calls, "none reached the Dev");
    // The control one byte away: the last answer that fits asks, then faults on
    // kproc's empty user half.
    TEST_EXPECT_EQ(lk_call_out(f, wo, T, 0, 1, UACCESS_USER_VA_TOP - sizeof(struct t_file_lock)),
                   -(s64)T_E_FAULT, "the highest answer in the user half");
    TEST_EXPECT_EQ(g_lk_calls, calls + 1, "reached the Dev");
    calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, wo, R | SYS_FILE_LOCK_TEST, 0, 1), -(s64)T_E_FAULT,
                   "a test needs no right; its answer faults on kproc's empty user half");
    TEST_EXPECT_EQ(g_lk_calls, calls + 1, "the test reached the Dev");
    TEST_EXPECT_EQ(g_lk_last_test, true, "as a test");
    TEST_EXPECT_EQ(g_lk_last_spoor, f->sp[wo], "on the open file itself");

    // Through the dispatcher: the arm reaches the handler.
    struct exception_context ctx;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_FILE_LOCK;
    ctx.regs[0] = (u64)f->fd[rw];
    ctx.regs[1] = 3;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], -(s64)T_E_INVAL, "SYS_FILE_LOCK reaches its handler");
}

void test_filelock_refusals(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int rw     = lk_open(&f, &g_lk_dev, 1, RW);
    int ro     = lk_open(&f, &g_lk_dev, 1, RIGHT_READ);
    int wo     = lk_open(&f, &g_lk_dev, 1, RIGHT_WRITE);
    int opath  = lk_open(&f, &g_lk_dev, 1, RW);
    int nolock = lk_open(&f, &g_lk_nolock_dev, 9, RW);
    int dead   = lk_open(&f, &g_lk_dev, 1, RW);
    hidx_t dead_fd = dead >= 0 ? f.fd[dead] : -1;
    lk_close_fd(&f, dead);
    hidx_t other = handle_alloc(f.p, KOBJ_PROCESS, RIGHT_READ, NULL);
    if (opath >= 0) f.sp[opath]->flag |= CWALKONLY;
    if (rw >= 0 && ro >= 0 && wo >= 0 && opath >= 0 && nolock >= 0 &&
        dead_fd >= 0 && other >= 0)
        refusals_body(&f, rw, ro, wo, opath, nolock, dead_fd, other);
    else
        test_fail("filelock: fixture allocation failed");
    if (other >= 0) handle_close(f.p, other);
    lk_teardown(&f, 9);
}

// -----------------------------------------------------------------------------
// The open-file owner: the Spoor's own fid, nothing minted.
// -----------------------------------------------------------------------------

static void file_owner_body(struct lk_fix *f, int a, int b, int c) {
    const u64 W = SYS_FILE_LOCK_WR, R = SYS_FILE_LOCK_RD, U = SYS_FILE_LOCK_UN;
    lk_sync_reset();
    TEST_EXPECT_EQ(lk_call(f, a, W, 0, 10), (s64)0, "open file A takes a write lock");
    TEST_EXPECT_EQ(g_lk_last_spoor, f->sp[a], "as itself");
    TEST_ASSERT(lk_synced(f->sp[a]) && lk_synced(f->sp[b]) && !lk_synced(f->sp[c]),
                "after landing the writes of every open of file 1, and only file 1");
    TEST_ASSERT(g_lk_sync_last_seq < g_lk_take_seq, "before the take was sent");
    lk_sync_reset();
    TEST_EXPECT_EQ(lk_call(f, b, R | SYS_FILE_LOCK_TEST, 0, 10), -(s64)T_E_FAULT,
                   "a test runs (its answer faults on kproc)");
    TEST_EXPECT_EQ(g_lk_sync_n, 0u, "and lands nothing");
    TEST_EXPECT_EQ(lk_call(f, b, R, 5, 10), -(s64)T_E_AGAIN,
                   "open file B of the same file conflicts");
    lk_sync_reset();
    TEST_EXPECT_EQ(lk_call(f, a, U, 0, 0), (s64)0, "A unlocks");
    TEST_ASSERT(lk_synced(f->sp[a]) && lk_synced(f->sp[b]),
                "after landing the writes of every open of the file");
    TEST_EXPECT_EQ(lk_call(f, b, R, 5, 10), (s64)0, "and B's read lock is granted");
    TEST_EXPECT_EQ(g_lk_mints, 0u, "the open-file owner mints nothing");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "and keeps no entry");
}

void test_filelock_file_owner(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    int c = lk_open(&f, &g_lk_dev, 2, RW);
    if (a >= 0 && b >= 0 && c >= 0) file_owner_body(&f, a, b, c);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 2);
}

// -----------------------------------------------------------------------------
// The Proc owner: one entry per file, shared by every fd of it in the Proc.
// -----------------------------------------------------------------------------

static void proc_owner_body(struct lk_fix *f, int a, int b, int c) {
    const u64 W = SYS_FILE_LOCK_WR, R = SYS_FILE_LOCK_RD, P = SYS_FILE_LOCK_PROC;
    TEST_EXPECT_EQ(lk_call(f, a, W | P, 0, 10), (s64)0, "the Proc takes a write lock through A");
    TEST_EXPECT_EQ(g_lk_mints, 1u, "one owner minted");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "one entry");
    struct Spoor *owner = g_lk_last_spoor;
    TEST_ASSERT(owner != f->sp[a] && owner != f->sp[b], "the lock is the owner's, not A's");
    TEST_EXPECT_EQ(lk_call(f, b, R, 0, 10), -(s64)T_E_AGAIN,
                   "an open-file lock conflicts with the Proc's own lock");
    lk_sync_reset();
    TEST_EXPECT_EQ(lk_call(f, b, R | P, 0, 10), (s64)0,
                   "the Proc's read lock through B replaces its write lock");
    TEST_ASSERT(lk_synced(f->sp[a]) && lk_synced(f->sp[b]) && !lk_synced(f->sp[c]),
                "after landing the writes of every open of file 1, and only file 1");
    TEST_ASSERT(g_lk_sync_last_seq < g_lk_take_seq, "before the conversion was sent");
    TEST_EXPECT_EQ(g_lk_last_spoor, owner, "on the same owner");
    TEST_EXPECT_EQ(g_lk_mints, 1u, "adopted, not minted");
    TEST_EXPECT_EQ(lk_call(f, b, R, 0, 10), (s64)0, "and now admits B's read lock");
    TEST_EXPECT_EQ(lk_call(f, c, W | P, 0, 0), (s64)0, "a second file");
    TEST_EXPECT_EQ(g_lk_mints, 2u, "mints a second owner");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 2u, "two entries");
    lk_sync_reset();
    TEST_EXPECT_EQ(lk_call(f, a, R | SYS_FILE_LOCK_TEST | P, 0, 10), -(s64)T_E_FAULT,
                   "a test runs (its answer faults on kproc)");
    TEST_EXPECT_EQ(g_lk_sync_n, 0u, "and lands nothing");
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_UN | P, 0, 0), (s64)0,
                   "the Proc unlocks file 1 through A");
    TEST_ASSERT(lk_synced(f->sp[a]) && lk_synced(f->sp[b]) && !lk_synced(f->sp[c]),
                "after landing the writes of every open of file 1, and only file 1");
    TEST_ASSERT(g_lk_sync_last_seq < g_lk_un_seq, "before the unlock was sent");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 2u, "the entry stays");
}

void test_filelock_proc_owner(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    int c = lk_open(&f, &g_lk_dev, 2, RW);
    if (a >= 0 && b >= 0 && c >= 0) proc_owner_body(&f, a, b, c);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 2);
    TEST_EXPECT_EQ(handle_lock_owner_count(f.p), 0u, "closing the files drops both entries");
}

// -----------------------------------------------------------------------------
// Any close of a descriptor of the file drops the Proc's locks on it, after
// landing every staged write the Proc can reach.
// -----------------------------------------------------------------------------

static void close_drops_body(struct lk_fix *f, int a, int b, int d, int op) {
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | SYS_FILE_LOCK_PROC, 0, 10), (s64)0,
                   "the Proc locks file 1 through A");
    struct Spoor *owner = g_lk_last_spoor;
    lk_close_fd(f, d);
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "closing another file's fd keeps the entry");
    TEST_EXPECT_EQ(g_lk_owner_closes, 0u, "and the owner");
    u32 un0 = g_lk_un_seq;
    // A navigation-only descriptor is no open of the file: the control one
    // variable away is B's close, below.
    lk_close_fd(f, op);
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "closing an O_PATH fd of file 1 keeps the entry");
    TEST_EXPECT_EQ(g_lk_un_seq, un0, "and sends no unlock");
    TEST_EXPECT_EQ(lk_held_on(1), 1u, "and the lock is held");
    lk_sync_reset();                 // the take already landed A's and B's writes
    lk_close_fd(f, b);
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "closing B (file 1) drops the entry");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "and clunks the owner");
    TEST_ASSERT(g_lk_un_seq != un0, "the whole file was unlocked first");
    TEST_EXPECT_EQ(g_lk_last_spoor, owner, "through the owner");
    TEST_EXPECT_EQ(g_lk_last_start, (u64)0, "from byte 0");
    TEST_EXPECT_EQ(g_lk_last_len, (u64)0, "to the end");
    TEST_ASSERT(lk_synced(f->sp[b]), "the closed open file's writes landed");
    TEST_ASSERT(lk_synced(f->sp[a]), "and A's, which stays open");
    TEST_ASSERT(g_lk_sync_last_seq < g_lk_un_seq, "before the unlock was sent");
    TEST_EXPECT_EQ(lk_held_on(1), 0u, "nothing is held on file 1");
}

void test_filelock_close_drops(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    int d = lk_open(&f, &g_lk_dev, 2, RW);
    int op = lk_open(&f, &g_lk_dev, 1, RW);
    if (op >= 0) f.sp[op]->flag |= CWALKONLY;
    if (a >= 0 && b >= 0 && d >= 0 && op >= 0) close_drops_body(&f, a, b, d, op);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 2);
}

// -----------------------------------------------------------------------------
// LOCK_WAIT retries until the server grants; without it, EAGAIN at once.
// -----------------------------------------------------------------------------

static void wait_body(struct lk_fix *f, int a) {
    struct exception_context ctx;
    g_lk_agains = 3;
    u32 calls = g_lk_calls;
    for (int i = 0; i < 31; i++) ctx.regs[i] = 0;
    ctx.regs[8] = SYS_FILE_LOCK;
    ctx.regs[0] = (u64)f->fd[a];
    ctx.regs[1] = SYS_FILE_LOCK_WR | SYS_FILE_LOCK_WAIT | SYS_FILE_LOCK_PROC;
    ctx.regs[3] = 10;
    TEST_SYSCALL_DISPATCH(&ctx);
    TEST_EXPECT_EQ((s64)ctx.regs[0], (s64)0, "a waiting lock is granted after three refusals");
    TEST_EXPECT_EQ(g_lk_calls - calls, 4u, "in four attempts");
    TEST_EXPECT_EQ(current_thread()->note_interruptible, false,
                   "the call's interruptibility ends with it");
    g_lk_agains = 1;
    calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | SYS_FILE_LOCK_PROC, 0, 10),
                   -(s64)T_E_AGAIN, "without WAIT a refusal is EAGAIN");
    TEST_EXPECT_EQ(g_lk_calls - calls, 1u, "after one attempt");
}

void test_filelock_wait(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    if (a >= 0) wait_body(&f, a);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 1);
}

// -----------------------------------------------------------------------------
// A take that is refused leaves no entry behind; a granted one stays.
// -----------------------------------------------------------------------------

static void failed_take_body(struct lk_fix *f, int a, int b) {
    const u64 W = SYS_FILE_LOCK_WR, P = SYS_FILE_LOCK_PROC;
    TEST_EXPECT_EQ(lk_call(f, a, W, 0, 0), (s64)0, "open file A holds the whole file");
    TEST_EXPECT_EQ(lk_call(f, b, W | P, 0, 1), -(s64)T_E_AGAIN, "the Proc's take is refused");
    TEST_EXPECT_EQ(g_lk_mints, 1u, "an owner was minted for it");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "and is gone: it never held a lock");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "clunked");
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_UN, 0, 0), (s64)0, "A unlocks");
    TEST_EXPECT_EQ(lk_call(f, b, W | P, 0, 1), (s64)0, "the Proc's take is granted");
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_UN | P, 0, 0), (s64)0, "and unlocked");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u,
                   "an owner that held a lock keeps its slot until a close");
}

void test_filelock_failed_take_spends_no_slot(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    if (a >= 0 && b >= 0) failed_take_body(&f, a, b);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 1);
}

// -----------------------------------------------------------------------------
// I-32: the table is bounded; past it a new file is ENOLCK, and the owner
// minted for it is released.
// -----------------------------------------------------------------------------

static void bound_body(struct lk_fix *f) {
    for (u32 i = 0; i < PROC_LOCK_OWNER_MAX; i++)
        TEST_EXPECT_EQ(lk_call(f, (int)i, SYS_FILE_LOCK_WR | SYS_FILE_LOCK_PROC, 0, 1),
                       (s64)0, "a file within the bound locks");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), (u32)PROC_LOCK_OWNER_MAX, "the table is full");
    TEST_EXPECT_EQ(lk_call(f, PROC_LOCK_OWNER_MAX, SYS_FILE_LOCK_WR | SYS_FILE_LOCK_PROC, 0, 1),
                   -(s64)T_E_NOLCK, "one more file is ENOLCK");
    TEST_EXPECT_EQ(g_lk_mints, (u32)PROC_LOCK_OWNER_MAX + 1, "its owner was minted");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "and released");
    TEST_EXPECT_EQ(lk_call(f, 0, SYS_FILE_LOCK_RD | SYS_FILE_LOCK_PROC, 0, 1), (s64)0,
                   "a file already in the table still locks");
}

void test_filelock_bound(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    bool ok = true;
    for (u32 i = 0; i <= PROC_LOCK_OWNER_MAX && ok; i++)
        ok = lk_open(&f, &g_lk_dev, 100 + i, RW) == (int)i;
    if (ok) bound_body(&f);
    else test_fail("filelock: fixture allocation failed");
    for (u32 i = 0; i < f.n; i++) lk_close_fd(&f, (int)i);
    TEST_EXPECT_EQ(handle_lock_owner_count(f.p), 0u, "closing every file empties the table");
    TEST_EXPECT_EQ(g_lk_owner_closes, (u32)PROC_LOCK_OWNER_MAX + 1, "and releases every owner");
}

// -----------------------------------------------------------------------------
// A close of the locking fd that lands while the take is on the wire: the
// close's whole-file unlock goes through the entry the call is using (which
// stays), the take is granted after it, and the call -- like Linux's
// fcntl_setlk -- finds its fd gone, unlocks its range and answers EBADF.
// -----------------------------------------------------------------------------

static void close_mid_call_body(struct lk_fix *f, int a) {
    g_lk_close_fd = f->fd[a];
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | SYS_FILE_LOCK_PROC, 0, 10),
                   -(s64)T_E_BADF, "the take whose fd closed mid-call is EBADF");
    f->fd[a] = -1;   // the fake closed it
    TEST_EXPECT_EQ(lk_held_on(1), 0u, "and holds nothing");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u,
                   "the entry the call was using survived the close");
    TEST_EXPECT_EQ(g_lk_owner_closes, 0u, "unclunked");
    int b = lk_open(f, &g_lk_dev, 1, RW);
    TEST_ASSERT(b >= 0, "fixture: a second fd of the file");
    lk_close_fd(f, b);
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "the next close of the file drops it");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "and clunks it");
}

void test_filelock_close_mid_call(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    if (a >= 0) close_mid_call_body(&f, a);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 1);
}

// -----------------------------------------------------------------------------
// F_GETLK for the Proc: with no entry, a fresh owner asks (not the open file's
// own fid, whose locks conflict with the Proc's) and is released.
// -----------------------------------------------------------------------------

static void proc_test_body(struct lk_fix *f, int a, int b) {
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR, 0, 10), (s64)0, "open file A holds a write lock");
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_RD | SYS_FILE_LOCK_TEST | SYS_FILE_LOCK_PROC, 0, 10),
                   -(s64)T_E_FAULT, "the Proc's test runs (its answer faults on kproc)");
    TEST_ASSERT(g_lk_last_spoor != f->sp[a] && g_lk_last_test, "asked as a fresh owner, not as A");
    TEST_EXPECT_EQ(g_lk_mints, 1u, "minted for the question");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "and released after it");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "no entry");
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_RD | SYS_FILE_LOCK_PROC, 20, 1), (s64)0,
                   "the Proc locks elsewhere in the file");
    struct Spoor *owner = g_lk_last_spoor;
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_RD | SYS_FILE_LOCK_TEST | SYS_FILE_LOCK_PROC, 0, 10),
                   -(s64)T_E_FAULT, "a test with an entry");
    TEST_EXPECT_EQ(g_lk_last_spoor, owner, "asks as the Proc's owner");
    TEST_EXPECT_EQ(g_lk_mints, 2u, "minting nothing more");
}

void test_filelock_proc_test_owner(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    if (a >= 0 && b >= 0) proc_test_body(&f, a, b);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 1);
}

// -----------------------------------------------------------------------------
// The table's life: rfork copies no entry; exec's close-on-exec close, a dup2
// onto the fd and a replace each drop it; the teardown clunks what is left.
// -----------------------------------------------------------------------------

// Take a Proc lock on c for p through the table, as the handler does.
static int lk_take(struct Proc *p, struct Spoor *c) {
    struct Spoor *o = NULL;
    int rc = handle_lock_owner_get(p, c, true, &o);
    if (rc != 0) return rc;
    rc = o->dev->lock(o, SYS_FILE_LOCK_WR, 0, 0, NULL);
    handle_lock_owner_put(p, o, rc == 0);
    return rc;
}

static void lifecycle_body(struct Proc *parent, struct Proc *child,
                           struct Spoor *s, hidx_t fd, hidx_t other) {
    TEST_EXPECT_EQ(lk_take(parent, s), 0, "the parent locks the file");
    TEST_EXPECT_EQ(handle_lock_owner_count(parent), 1u, "one entry");
    TEST_ASSERT(handle_table_copy_into(child, parent) >= 1, "rfork copies the fds");
    TEST_EXPECT_EQ(handle_lock_owner_count(child), 0u, "and no lock owner");

    TEST_ASSERT(handle_set_cloexec(parent, fd, true) == 0, "fixture: cloexec");
    TEST_EXPECT_EQ(handle_close_on_exec(parent), 1, "exec closes the fd");
    TEST_EXPECT_EQ(handle_lock_owner_count(parent), 0u, "which drops the entry");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "and clunks the owner");

    // dup2 onto a descriptor of the locked file closes it.
    hidx_t fd2 = handle_dup_to(parent, other, 7, false);
    TEST_ASSERT(fd2 == 7, "fixture: an fd of the file at 7");
    TEST_EXPECT_EQ(lk_take(parent, s), 0, "the parent locks it again");
    hidx_t z = handle_alloc(parent, KOBJ_PROCESS, RIGHT_READ, NULL);
    TEST_ASSERT(z >= 0, "fixture: another handle");
    TEST_EXPECT_EQ(handle_dup_to(parent, z, fd2, false), fd2, "dup2 over fd 7");
    TEST_EXPECT_EQ(handle_lock_owner_count(parent), 0u, "drops the entry");
    TEST_EXPECT_EQ(g_lk_owner_closes, 2u, "and its owner");
    handle_close(parent, z);
    handle_close(parent, fd2);

    // The teardown clunks a live entry.
    TEST_EXPECT_EQ(lk_take(child, s), 0, "the child locks the file");
    TEST_EXPECT_EQ(handle_lock_owner_count(child), 1u, "one entry in the child");
}

void test_filelock_fork_exec_dup_teardown(void) {
    lk_reset();
    struct Proc *parent = proc_alloc();
    struct Proc *child  = proc_alloc();
    struct Spoor *s = spoor_alloc(&g_lk_dev);
    hidx_t fd = -1, other = -1;
    if (s) {
        s->qid.path = 1;
        spoor_ref(s);   // one for each of the parent's two fds
        if (parent) {
            fd    = handle_alloc(parent, KOBJ_SPOOR, RW, s);
            other = handle_alloc(parent, KOBJ_SPOOR, RW, s);
        }
        if (fd < 0)    spoor_clunk(s);
        if (other < 0) spoor_clunk(s);
    }
    if (parent && child && fd >= 0 && other >= 0)
        lifecycle_body(parent, child, s, fd, other);
    else
        test_fail("filelock: fixture allocation failed");
    u32  closes = g_lk_owner_closes;
    bool held   = child && handle_lock_owner_count(child) == 1;
    if (child)  { child->state = PROC_STATE_ZOMBIE;  proc_free(child); }
    bool clunked = g_lk_owner_closes == closes + 1;
    if (parent) { parent->state = PROC_STATE_ZOMBIE; proc_free(parent); }
    if (held) TEST_ASSERT(clunked, "the child's teardown clunked its owner");
}

// -----------------------------------------------------------------------------
// A dead file's entry: its fid answers ESTALE, and its number may now name the
// file the call is about (a server reuses inode numbers).
// -----------------------------------------------------------------------------

static void stale_entry_body(struct lk_fix *f, int a, int b) {
    const u64 P = SYS_FILE_LOCK_PROC;
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | P, 0, 1), (s64)0, "the Proc locks the file through a");
    struct Spoor *dead = g_lk_last_spoor;
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_WR | P, 10, 1), (s64)0, "control: a take through b");
    TEST_ASSERT(g_lk_last_spoor == dead && g_lk_mints == 1u, "control: it shares the live entry");

    g_lk_stale = dead;
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_WR | P, 20, 1), (s64)0,
                   "once the entry's fid answers ESTALE, a take is still granted");
    struct Spoor *fresh = g_lk_last_spoor;
    TEST_EXPECT_EQ(g_lk_mints, 2u, "through a fresh owner");
    TEST_EXPECT_EQ(g_lk_owner_closes, 1u, "the dead entry's owner is closed");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "and only the fresh entry is left");

    g_lk_stale = fresh;
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_UN | P, 0, 0), (s64)0,
                   "an unlock through a dead entry answers 0");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "and retires it");
    TEST_EXPECT_EQ(g_lk_owner_closes, 2u, "closing its owner");
    g_lk_stale = NULL;

    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_WR | P, 0, 1), (s64)0, "a fresh entry");
    g_lk_stale = g_lk_last_spoor;
    u32 m = g_lk_mints;
    TEST_EXPECT_EQ(lk_call(f, b, SYS_FILE_LOCK_RD | SYS_FILE_LOCK_TEST | P, 0, 1), -(s64)T_E_FAULT,
                   "a test through a dead entry runs (its answer faults on kproc)");
    TEST_EXPECT_EQ(g_lk_mints, m + 1u, "asked as a temporary owner");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "with the dead entry retired");
    g_lk_stale = NULL;

    // The descriptor's own file has died too: one retry, then ESTALE.
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | P, 0, 1), (s64)0, "an entry again");
    g_lk_stale_path = 1;
    u32 calls = g_lk_calls;
    m = g_lk_mints;
    TEST_EXPECT_EQ(lk_call(f, a, SYS_FILE_LOCK_WR | P, 5, 1), -(s64)T_E_STALE,
                   "a take on a file that died answers ESTALE");
    TEST_EXPECT_EQ(g_lk_calls, calls + 2u, "after exactly one retry");
    TEST_EXPECT_EQ(g_lk_mints, m + 1u, "through one fresh mint");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "leaving no entry");
    g_lk_stale_path = 0;
}

void test_filelock_stale_entry_retired(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a = lk_open(&f, &g_lk_dev, 1, RW);
    int b = lk_open(&f, &g_lk_dev, 1, RW);
    if (a >= 0 && b >= 0) stale_entry_body(&f, a, b);
    else test_fail("filelock: fixture allocation failed");
    g_lk_stale = NULL;
    g_lk_stale_path = 0;
    lk_teardown(&f, 1);
}

// -----------------------------------------------------------------------------
// A file's number outlives it: Stratum gives a dead file's inode number to the
// next file created, so a descriptor of a dead file and an entry for the file
// that took its number match by number alone. The generation tells them apart.
// -----------------------------------------------------------------------------

static void reused_number_body(struct lk_fix *f, int y, int y2, int x, int xk, int xi) {
    const u64 P = SYS_FILE_LOCK_PROC;
    lk_set_gen(f->sp[y], 2);
    lk_set_gen(f->sp[y2], 2);
    lk_set_gen(f->sp[x], 1);
    f->sp[xk]->qid.vers = 1;        // learned while its file was alive
    g_lk_dead = f->sp[x];
    TEST_EXPECT_EQ(lk_call(f, y, SYS_FILE_LOCK_WR | P, 0, 0), (s64)0, "the Proc locks file Y");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "one entry, for Y");
    TEST_EXPECT_EQ(lk_held_on(7), 1u, "Y's lock is held");

    u32 m = g_lk_mints, calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, x, SYS_FILE_LOCK_WR | P, 0, 0), -(s64)T_E_STALE,
                   "a Proc lock through the dead file's descriptor answers ESTALE");
    TEST_ASSERT(g_lk_mints == m && g_lk_calls == calls,
                "and reaches neither Y's entry nor the server's table");
    TEST_EXPECT_EQ(lk_call(f, x, SYS_FILE_LOCK_UN | P, 0, 0), -(s64)T_E_STALE,
                   "so does its unlock, which leaves Y's lock alone");
    TEST_EXPECT_EQ(lk_held_on(7), 1u, "Y's lock is still held");

    u32 asked = g_lk_idents;
    lk_close_fd(f, x);
    TEST_EXPECT_EQ(g_lk_idents, asked + 1u, "its close asks which file it named");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "and keeps Y's entry");
    TEST_EXPECT_EQ(lk_held_on(7), 1u, "and Y's lock");
    asked = g_lk_idents;
    lk_close_fd(f, xk);
    TEST_EXPECT_EQ(g_lk_idents, asked, "a dead file's descriptor that knows its generation needs no question");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "and its close keeps Y's entry too");

    // A question the server could not answer names no file either.
    g_lk_dead       = f->sp[xi];
    g_lk_ident_fail = -T_E_IO;
    lk_close_fd(f, xi);
    g_lk_ident_fail = 0;
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 1u, "a close that cannot learn its file keeps Y's entry");
    TEST_EXPECT_EQ(lk_held_on(7), 1u, "and Y's lock");

    // The control, one variable away: a live descriptor of Y that never locked.
    lk_close_fd(f, y2);
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), 0u, "closing another descriptor of Y drops the entry");
    TEST_EXPECT_EQ(lk_held_on(7), 0u, "and Y's lock");
}

void test_filelock_reused_number(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int y  = lk_open(&f, &g_lk_dev, 7, RW);
    int y2 = lk_open(&f, &g_lk_dev, 7, RW);
    int x  = lk_open(&f, &g_lk_dev, 7, RW);
    int xk = lk_open(&f, &g_lk_dev, 7, RW);
    int xi = lk_open(&f, &g_lk_dev, 7, RW);
    if (y >= 0 && y2 >= 0 && x >= 0 && xk >= 0 && xi >= 0)
        reused_number_body(&f, y, y2, x, xk, xi);
    else test_fail("filelock: fixture allocation failed");
    g_lk_dead = NULL;
    lk_teardown(&f, 7);
}

// -----------------------------------------------------------------------------
// Before a take: every open of the file reads what the server holds.
// -----------------------------------------------------------------------------

// Before every take, for either owner, the call has each open of the file the
// Proc holds read what the server holds (Dev.lock_sync ACQUIRE: dev9p's cached
// opens leave their snapshots), so the lock covers what any of them reads. A
// test and an unlock never ask. An open that cannot comply refuses the take
// with its errno: no lock reaches the server, and a Proc take spends no slot.
static void acquire_syncs_body(struct lk_fix *f, int a, int b, int o, int o2) {
    const u64 W = SYS_FILE_LOCK_WR, R = SYS_FILE_LOCK_RD, U = SYS_FILE_LOCK_UN;
    const u64 P = SYS_FILE_LOCK_PROC;
    TEST_EXPECT_EQ(lk_call(f, a, W, 0, 4), (s64)0, "an open-file take");
    TEST_ASSERT(lk_acquired(f->sp[a]) && lk_acquired(f->sp[b]),
                "has every open of the file read the server");
    TEST_ASSERT(!lk_acquired(f->sp[o]) && !lk_acquired(f->sp[o2]), "and no other file's");
    TEST_ASSERT(g_lk_take_seq != 0 && g_lk_acq_last_seq < g_lk_take_seq,
                "before the take is sent");
    TEST_EXPECT_EQ(lk_call(f, a, U, 0, 4), (s64)0, "its unlock");
    lk_acq_reset();
    u32 calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, b, R | SYS_FILE_LOCK_TEST, 0, 4), -(s64)T_E_FAULT,
                   "a test (its answer faults on kproc's empty user half)");
    TEST_EXPECT_EQ(lk_call(f, b, U | P, 0, 4), (s64)0, "a Proc unlock of nothing");
    TEST_EXPECT_EQ(g_lk_calls, calls + 1u, "the test reached the Dev");
    TEST_EXPECT_EQ(g_lk_acq_n, 0u, "neither a test nor an unlock asks");

    TEST_EXPECT_EQ(lk_call(f, b, R | P, 0, 4), (s64)0, "a Proc take");
    TEST_ASSERT(lk_acquired(f->sp[a]) && lk_acquired(f->sp[b]) &&
                !lk_acquired(f->sp[o]) && !lk_acquired(f->sp[o2]),
                "has every open of that file, and only that file, read the server");
    TEST_ASSERT(g_lk_take_seq != 0 && g_lk_acq_last_seq < g_lk_take_seq,
                "before the take is sent");
    TEST_EXPECT_EQ(lk_call(f, b, U | P, 0, 4), (s64)0, "its unlock");

    // File 8 has no entry yet. Its second open cannot comply.
    g_lk_acq_fail    = f->sp[o2];
    g_lk_acq_fail_rc = -T_E_IO;
    u32 entries = handle_lock_owner_count(f->p);
    calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, o, W, 0, 0), -(s64)T_E_IO,
                   "an open that cannot read the server refuses an open-file take");
    TEST_EXPECT_EQ(lk_call(f, o, W | P, 0, 0), -(s64)T_E_IO, "and a Proc take");
    TEST_EXPECT_EQ(g_lk_calls, calls, "neither reached the server");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), entries,
                   "the refused Proc take spent no slot");
    // The control, one variable away: the same takes once that open complies.
    g_lk_acq_fail = NULL;
    TEST_EXPECT_EQ(lk_call(f, o, W, 0, 0), (s64)0, "the open-file take, once it complies");
    TEST_EXPECT_EQ(lk_call(f, o, U, 0, 0), (s64)0, "its unlock");
    TEST_EXPECT_EQ(lk_call(f, o, W | P, 0, 0), (s64)0, "the Proc take");
    TEST_EXPECT_EQ(handle_lock_owner_count(f->p), entries + 1u, "spends its slot");
    // A peer that can no longer reach the file (ESTALE) keeps its snapshot, and
    // a Proc take through the descriptor that can reach it stands. That
    // descriptor's own ESTALE is the file's death, and refuses.
    TEST_EXPECT_EQ(lk_call(f, o, U | P, 0, 0), (s64)0, "the unlock");
    g_lk_acq_fail    = f->sp[o2];
    g_lk_acq_fail_rc = -T_E_STALE;
    TEST_EXPECT_EQ(lk_call(f, o, W | P, 0, 0), (s64)0, "a peer's ESTALE refuses no take");
    TEST_EXPECT_EQ(lk_call(f, o, U | P, 0, 0), (s64)0, "its unlock");
    g_lk_acq_fail = f->sp[o];
    calls = g_lk_calls;
    TEST_EXPECT_EQ(lk_call(f, o, W | P, 0, 0), -(s64)T_E_STALE, "its own ESTALE refuses");
    TEST_EXPECT_EQ(g_lk_calls, calls, "before the server");
    g_lk_acq_fail = NULL;
}

void test_filelock_acquire_syncs(void) {
    struct lk_fix f;
    lk_fix_init(&f);
    int a  = lk_open(&f, &g_lk_dev, 7, RW);
    int b  = lk_open(&f, &g_lk_dev, 7, RIGHT_READ);
    int o  = lk_open(&f, &g_lk_dev, 8, RW);
    int o2 = lk_open(&f, &g_lk_dev, 8, RIGHT_READ);
    if (a >= 0 && b >= 0 && o >= 0 && o2 >= 0) acquire_syncs_body(&f, a, b, o, o2);
    else test_fail("filelock: fixture allocation failed");
    lk_teardown(&f, 8);
}

// -----------------------------------------------------------------------------
// The last descriptor, not the last reference: every kind of slot counts, nothing
// else does, and that close drops what the open file holds with a holder alive.
// -----------------------------------------------------------------------------

static void last_descriptor_count_body(struct Proc *parent, struct Proc *child,
                                       struct Spoor *s, struct Spoor *t, hidx_t fd) {
    hidx_t d1 = handle_dup(parent, fd, RW);
    hidx_t d2 = handle_dup_to(parent, fd, 20, false);
    hidx_t d3 = handle_dup_posix(parent, fd, 30, false);
    TEST_ASSERT(d1 >= 0 && d2 == 20 && d3 >= 30, "fixture: three more descriptors");
    TEST_ASSERT(handle_table_copy_into(child, parent) == 4, "rfork copies all four");

    // A replace: the slot leaves s, and comes to name t.
    TEST_EXPECT_EQ(handle_replace(parent, d2, KOBJ_SPOOR, RW, t), 0, "a replace");
    TEST_EXPECT_EQ(g_lk_fd_closes, 0u, "no close yet: every slot still counted");
    spoor_ref(s);
    TEST_EXPECT_EQ(handle_replace(parent, d2, KOBJ_SPOOR, RW, s), 0, "and back");
    TEST_EXPECT_EQ(g_lk_fd_closes, 1u, "t's only descriptor left");
    TEST_ASSERT(g_lk_fd_close_last == t, "its last close ran");
    TEST_EXPECT_EQ(g_lk_closes, 1u, "and with nothing else holding it, its last reference");

    // dup2 onto a descriptor of the same open file: one goes, one comes.
    TEST_EXPECT_EQ(handle_dup_to(parent, fd, d2, false), d2, "dup2 onto its own open file");
    TEST_EXPECT_EQ(g_lk_fd_closes, 1u, "is no close");

    // Closing all but one, in each table.
    TEST_EXPECT_EQ(handle_close(parent, d1), 0, "close");
    TEST_EXPECT_EQ(handle_close(parent, d2), 0, "close");
    TEST_EXPECT_EQ(handle_close(parent, d3), 0, "close");
    TEST_EXPECT_EQ(handle_close(child, d1), 0, "the child's");
    TEST_EXPECT_EQ(handle_close(child, d2), 0, "the child's");
    TEST_EXPECT_EQ(handle_close(child, d3), 0, "the child's");
    TEST_EXPECT_EQ(handle_close(parent, fd), 0, "the parent's last");
    TEST_EXPECT_EQ(g_lk_fd_closes, 1u, "the child still has one");
}

// The fixture hands the caller's reference on to the first slot.
void test_filelock_last_descriptor_closes(void) {
    lk_reset();
    g_lk_fd_closes = g_lk_closes = g_lk_held_close_seq = 0;
    g_lk_fd_close_last = NULL;
    struct Proc *parent = proc_alloc();
    struct Proc *child  = proc_alloc();
    struct Spoor *s = spoor_alloc(&g_lk_dev);
    struct Spoor *t = spoor_alloc(&g_lk_dev);
    hidx_t fd = -1;
    if (s && parent) fd = handle_alloc(parent, KOBJ_SPOOR, RW, s);
    if (fd >= 0 && child && t) {
        s->qid.path = 1;
        t->qid.path = 2;
        last_descriptor_count_body(parent, child, s, t, fd);
    } else {
        test_fail("filelock: fixture allocation failed");
    }
    // An exit is a close of each slot.
    u32 fc = g_lk_fd_closes;
    if (child) { child->state = PROC_STATE_ZOMBIE; proc_free(child); }
    TEST_EXPECT_EQ(g_lk_fd_closes, fc + 1, "the child's exit is the last close");
    TEST_ASSERT(g_lk_fd_close_last == s, "of the open file");
    if (parent) { parent->state = PROC_STATE_ZOMBIE; proc_free(parent); }

    struct lk_fix f;
    lk_fix_init(&f);
    g_lk_fd_closes = g_lk_closes = g_lk_held_close_seq = 0;

    // A holder, as a read-only map's pin: the close drops the lock regardless.
    int a = lk_open(&f, &g_lk_dev, 3, RW);
    TEST_ASSERT(a >= 0, "fixture: a file");
    struct Spoor *pin = f.sp[a];
    TEST_EXPECT_EQ(lk_call(&f, a, SYS_FILE_LOCK_WR, 0, 0), (s64)0, "an open-file lock");
    spoor_ref(pin);
    lk_close_fd(&f, a);
    TEST_EXPECT_EQ(lk_held_on(3), 0u, "the last close drops it with a holder alive");
    TEST_EXPECT_EQ(g_lk_closes, 0u, "and the holder keeps the Spoor");
    spoor_clunk(pin);
    TEST_EXPECT_EQ(g_lk_closes, 1u, "until it lets go");
    TEST_EXPECT_EQ(g_lk_fd_closes, 1u, "with no second close");

    // A lock call's own reference is no descriptor either.
    a = lk_open(&f, &g_lk_dev, 3, RW);
    TEST_ASSERT(a >= 0, "fixture: a file");
    struct Handle h;
    TEST_EXPECT_EQ(handle_get(f.p, f.fd[a], &h), 0, "a call's snapshot");
    u32 fc0 = g_lk_fd_closes;
    lk_close_fd(&f, a);
    TEST_EXPECT_EQ(g_lk_fd_closes, fc0 + 1, "does not delay the last close");
    handle_put(&h);
    TEST_EXPECT_EQ(g_lk_fd_closes, fc0 + 1, "and its release is no second one");

    // The close that drops a lock lands every open's writes first. One that
    // leaves a descriptor open drops nothing and lands nothing.
    a = lk_open(&f, &g_lk_dev, 4, RW);
    int b = lk_open(&f, &g_lk_dev, 4, RW);
    int c = lk_open(&f, &g_lk_dev, 4, RW);
    TEST_ASSERT(a >= 0 && b >= 0 && c >= 0, "fixture: three opens of a file");
    TEST_EXPECT_EQ(lk_call(&f, a, SYS_FILE_LOCK_WR, 0, 0), (s64)0, "a holds an open-file lock");
    hidx_t da = handle_dup(f.p, f.fd[a], RW);
    TEST_ASSERT(da >= 0, "fixture: a dup of a");
    lk_sync_reset();
    TEST_EXPECT_EQ(handle_close(f.p, da), 0, "the dup's close");
    TEST_EXPECT_EQ(g_lk_sync_n, 0u, "lands nothing: a's descriptor remains");
    TEST_EXPECT_EQ(lk_held_on(4), 1u, "and the lock stands");
    lk_close_fd(&f, c);
    TEST_EXPECT_EQ(g_lk_sync_n, 0u, "a close holding no lock lands nothing (control)");
    lk_sync_reset();
    lk_close_fd(&f, a);
    TEST_ASSERT(lk_synced(f.sp[b]), "the last close lands b's writes");
    TEST_ASSERT(g_lk_sync_last_seq < g_lk_held_close_seq, "before it gives the lock up");
    TEST_EXPECT_EQ(lk_held_on(4), 0u, "which it does");
    lk_close_fd(&f, b);

    // A take that fails with its outcome unknown may hold a grant too: the close
    // runs again, and the failure stands.
    a = lk_open(&f, &g_lk_dev, 6, RW);
    TEST_ASSERT(a >= 0, "fixture: a file");
    g_lk_close_fd = f.fd[a];
    g_lk_stale_path = 6;
    u32 fc1 = g_lk_fd_closes;
    TEST_EXPECT_EQ(lk_call(&f, a, SYS_FILE_LOCK_WR, 0, 10), -(s64)T_E_STALE,
                   "a failed take answers its own failure");
    g_lk_stale_path = 0;
    TEST_EXPECT_EQ(g_lk_fd_closes, fc1 + 2, "and its re-check runs the close again");
    f.fd[a] = -1;   // the fake closed it

    // The last descriptor closes while a take is on the wire. A Proc lock has its
    // re-check; an open-file lock had no close left to drop it.
    a = lk_open(&f, &g_lk_dev, 5, RW);
    TEST_ASSERT(a >= 0, "fixture: a file");
    g_lk_close_fd = f.fd[a];
    TEST_EXPECT_EQ(lk_call(&f, a, SYS_FILE_LOCK_WR, 0, 10), -(s64)T_E_BADF,
                   "a take whose last descriptor closed mid-call is EBADF");
    f.fd[a] = -1;   // the fake closed it
    TEST_EXPECT_EQ(lk_held_on(5), 0u, "and holds nothing");
    a = lk_open(&f, &g_lk_dev, 5, RW);
    TEST_ASSERT(a >= 0, "fixture: a file");
    hidx_t kept = handle_dup(f.p, f.fd[a], RW);
    TEST_ASSERT(kept >= 0, "fixture: its dup");
    g_lk_close_fd = f.fd[a];
    TEST_EXPECT_EQ(sys_file_lock_handler((u64)kept, SYS_FILE_LOCK_WR, 0, 10, 0), (s64)0,
                   "a descriptor remains: the take stands (control)");
    f.fd[a] = -1;
    TEST_EXPECT_EQ(lk_held_on(5), 1u, "and holds");
    TEST_EXPECT_EQ(handle_close(f.p, kept), 0, "its close");
    TEST_EXPECT_EQ(lk_held_on(5), 0u, "drops it");

    // An exit lands the writes of a slot above the lock's before it gives the lock
    // up, whatever the slots' order.
    lk_reset();
    g_lk_fd_closes = g_lk_held_close_seq = 0;
    struct Proc *ex = proc_alloc();
    struct Spoor *la = spoor_alloc(&g_lk_dev);
    struct Spoor *lb = spoor_alloc(&g_lk_dev);
    if (ex && la && lb) {
        la->qid.path = lb->qid.path = 6;
        TEST_ASSERT(handle_alloc(ex, KOBJ_SPOOR, RW, la) >= 0, "fixture: a");
        TEST_ASSERT(handle_alloc(ex, KOBJ_SPOOR, RW, lb) >= 0, "fixture: b, above it");
        TEST_EXPECT_EQ(la->dev->lock(la, SYS_FILE_LOCK_WR, 0, 0, NULL), 0, "a's lock");
        ex->state = PROC_STATE_ZOMBIE;
        proc_free(ex);
        TEST_ASSERT(lk_synced(lb), "the exit lands b's writes");
        TEST_ASSERT(g_lk_sync_last_seq < g_lk_held_close_seq, "before a gives the lock up");
        TEST_EXPECT_EQ(lk_held_on(6), 0u, "which it does");
    } else {
        test_fail("filelock: fixture allocation failed");
    }
    lk_teardown(&f, 5);
}
