// Per-thread /proc files and devproc readdir (thread-model C7, DEBUG-FS-DESIGN
// 5h).
//
//   devproc.thread_files_name_their_thread  each tid's regs, fpregs and kregs
//                                           read and write that thread's state;
//                                           the Proc-level files stay on main;
//                                           an exiting or departed thread has no
//                                           files
//   devproc.thread_step_names_its_thread    step <tid> arms the named thread and
//                                           no other; the operand's parse
//   devproc.thread_dir_gated                a stranger can neither list the
//                                           threads nor walk to one; the owner
//                                           and a CAP_DEBUG holder can; a caller
//                                           running as none lists only its own pid
//   devproc.readdir_lists_every_dir         the root, pid, thread and tid
//                                           directories list completely and
//                                           resume from their cookies, past one
//                                           pass's batch; ".." climbs one level
//
// The targets are synthetic: a proc_alloc'd Proc linked into the table, with
// hand-built threads parked on their own debug_rendez, as test_devproc.c's
// debug tests build them. Every leg records its result first and asserts after
// the fixture is unlinked and freed, because TEST_ASSERT returns and would
// otherwise leave the table pointing at this frame's threads.

#include "test.h"

#include "../../arch/arm64/exception.h"   // struct exception_context: the synthetic frames
#include "../../mm/phys.h"                // alloc_pages / free_pages: the synthetic kstacks
#include "../../arch/arm64/mmu.h"         // pa_to_kva

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
bool devproc_parse_step_tid_for_test(const char *s, long n, int *tid_out);
int  devproc_step_arm_for_test(int pid, int tid, struct Proc *caller, struct Spoor *ctl);

void test_devproc_thread_files_name_their_thread(void);
void test_devproc_thread_step_names_its_thread(void);
void test_devproc_thread_dir_gated(void);
void test_devproc_readdir_lists_every_dir(void);

// =============================================================================
// Helpers.
// =============================================================================

static size_t dt_udec(char *out, u64 v) {
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

static bool dt_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static struct Spoor *dt_step(struct Spoor *c, const char *name) {
    const char *names[1] = { name };
    struct Walkqid *wq = devproc.walk(c, NULL, names, 1);
    if (!wq) return NULL;
    struct Spoor *r = wq->spoor;
    bool ok = wq->nqid == 1;
    walkqid_free(wq);
    if (!ok) { spoor_unref(r); return NULL; }
    return r;
}

// Walk from the root through `n` names; NULL on any miss.
static struct Spoor *dt_walk(const char *const *names, int n) {
    struct Spoor *c = devproc.attach("");
    for (int i = 0; c && i < n; i++) {
        struct Spoor *next = dt_step(c, names[i]);
        spoor_unref(c);
        c = next;
    }
    return c;
}

// /proc/<pid>/thread/<tid>/<file> (file NULL: the tid directory; tid < 0: the
// thread directory), opened when `omode` >= 0.
static struct Spoor *dt_open(int pid, int tid, const char *file, int omode) {
    char ps[12], ts[12];
    dt_udec(ps, (u64)pid);
    const char *names[4] = { ps, "thread", ts, file };
    int n = 2;
    if (tid >= 0) {
        dt_udec(ts, (u64)tid);
        n = file ? 4 : 3;
    }
    struct Spoor *c = dt_walk(names, n);
    if (c && omode >= 0 && !devproc.open(c, omode)) {
        spoor_unref(c);
        return NULL;
    }
    return c;
}

static struct Spoor *dt_open_pidfile(int pid, const char *file, int omode) {
    char ps[12];
    dt_udec(ps, (u64)pid);
    const char *names[2] = { ps, file };
    struct Spoor *c = dt_walk(names, 2);
    if (c && !devproc.open(c, omode)) {
        spoor_unref(c);
        return NULL;
    }
    return c;
}

struct dt_ent {
    char name[16];
    u64  cookie;
    u64  qpath;
    u32  qvers;
    u8   qtype;
};

// The most entries one readdir call of the last dt_list returned.
static int g_dt_list_max_run;

// List directory `c` through readdir in runs of at most `chunk` bytes, resuming
// from each run's last cookie as SYS_READDIR does. Returns the entry count (the
// first `max` are kept), or the first negative return. *increasing says every
// cookie exceeded the one before it.
static int dt_list(struct Spoor *c, long chunk, struct dt_ent *ents, int max,
                   bool *increasing) {
    static u8 buf[1024];
    if (chunk > (long)sizeof(buf)) chunk = (long)sizeof(buf);
    s64 off = 0;
    int n = 0;
    u64 last = 0;
    *increasing = true;
    g_dt_list_max_run = 0;
    for (int pass = 0; pass < 4096; pass++) {
        long got = devproc.readdir(c, buf, chunk, off);
        if (got < 0)  return (int)got;
        if (got == 0) return n;
        long pos = 0;
        int run = 0;
        while (pos + 24 <= got) {
            u32 nlen = (u32)buf[pos + 22] | ((u32)buf[pos + 23] << 8);
            if (pos + 24 + (long)nlen > got) break;
            u64 cookie = 0, qpath = 0;
            u32 qvers = 0;
            for (int b = 0; b < 8; b++) cookie |= (u64)buf[pos + 13 + b] << (8 * b);
            for (int b = 0; b < 8; b++) qpath  |= (u64)buf[pos + 5 + b] << (8 * b);
            for (int b = 0; b < 4; b++) qvers  |= (u32)buf[pos + 1 + b] << (8 * b);
            if (cookie <= last) *increasing = false;
            last = cookie;
            if (n < max) {
                u32 k = nlen < 15 ? nlen : 15;
                for (u32 i = 0; i < k; i++) ents[n].name[i] = (char)buf[pos + 24 + (long)i];
                ents[n].name[k] = '\0';
                ents[n].cookie = cookie;
                ents[n].qpath  = qpath;
                ents[n].qvers  = qvers;
                ents[n].qtype  = buf[pos];
            }
            n++;
            run++;
            pos += 24 + (long)nlen;
        }
        if (run > g_dt_list_max_run) g_dt_list_max_run = run;
        off = (s64)last;
    }
    return -999;   // never reached its end
}

// =============================================================================
// The fixture: a stopped target with main (A), a newer thread (B) and an exited
// newest thread (X) at the head, A and B parked with frames of their own.
// =============================================================================

static struct Thread g_dt_a, g_dt_b, g_dt_x;

struct dt_fix {
    struct Proc *tgt;
    struct page *ka, *kb;
    struct exception_context *fa, *fb;
    int pid, ta, tb, tx;
};

static struct exception_context *dt_thread_init(struct Thread *th, struct page *k,
                                                int tid, u64 tag) {
    for (size_t i = 0; i < sizeof(*th); i++) ((u8 *)th)[i] = 0;
    u8 *kbase = (u8 *)pa_to_kva(page_to_pa(k));
    th->magic             = THREAD_MAGIC;
    th->tid               = tid;
    th->state             = THREAD_SLEEPING;
    th->kstack_base       = kbase;
    th->kstack_size       = THREAD_KSTACK_TOTAL_SIZE;
    th->on_cpu            = false;
    th->rendez_blocked_on = &th->debug_rendez;
    th->ctx.tpidr_el0     = tag << 8;
    th->ctx.fpsr          = (u32)tag;
    struct exception_context *f = (struct exception_context *)
        (kbase + THREAD_KSTACK_TOTAL_SIZE - EXCEPTION_CTX_SIZE);
    for (int i = 0; i < 31; i++) f->regs[i] = (tag << 12) + (u64)i;
    f->sp  = tag << 20;
    f->elr = tag << 16;
    th->debug_trapframe = f;
    return f;
}

static bool dt_fix_up(struct dt_fix *f, u32 principal) {
    for (size_t i = 0; i < sizeof(*f); i++) ((u8 *)f)[i] = 0;
    f->tgt = proc_alloc();
    if (!f->tgt) return false;
    f->ka = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    f->kb = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!f->ka || !f->kb) {
        if (f->ka) free_pages(f->ka, THREAD_KSTACK_TOTAL_ORDER);
        if (f->kb) free_pages(f->kb, THREAD_KSTACK_TOTAL_ORDER);
        f->tgt->state = PROC_STATE_ZOMBIE;
        proc_free(f->tgt);
        f->tgt = NULL;
        return false;
    }
    f->pid = f->tgt->pid;
    f->ta  = f->pid;          // a main thread's tid is its pid (ARCH 7.3.4)
    f->tb  = f->pid + 2;
    f->tx  = f->pid + 4;
    f->tgt->principal_id = principal;
    f->tgt->state        = PROC_STATE_ALIVE;
    f->fa = dt_thread_init(&g_dt_a, f->ka, f->ta, 0xA);
    f->fb = dt_thread_init(&g_dt_b, f->kb, f->tb, 0xB);
    for (size_t i = 0; i < sizeof(g_dt_x); i++) ((u8 *)&g_dt_x)[i] = 0;
    g_dt_x.magic = THREAD_MAGIC;
    g_dt_x.tid   = f->tx;
    g_dt_x.state = THREAD_EXITING;
    // Head-inserted, newest first.
    g_dt_x.next_in_proc = &g_dt_b;
    g_dt_b.next_in_proc = &g_dt_a;
    g_dt_a.next_in_proc = NULL;
    f->tgt->threads        = &g_dt_x;
    f->tgt->debug_stop_req = 1;
    proc_test_link(f->tgt);
    return true;
}

static void dt_fix_down(struct dt_fix *f) {
    if (!f->tgt) return;
    proc_test_unlink(f->tgt);
    f->tgt->threads     = NULL;
    f->tgt->debug_owner = NULL;
    free_pages(f->ka, THREAD_KSTACK_TOTAL_ORDER);
    free_pages(f->kb, THREAD_KSTACK_TOTAL_ORDER);
    f->tgt->state = PROC_STATE_ZOMBIE;
    proc_free(f->tgt);
    f->tgt = NULL;
}

static long dt_read_pc(struct Spoor *c, u64 *pc) {
    struct t_user_regs ur;
    for (size_t i = 0; i < sizeof(ur); i++) ((u8 *)&ur)[i] = 0;
    long rc = c ? devproc.read(c, &ur, (long)sizeof(ur), 0) : -2;
    *pc = ur.pc;
    return rc;
}

// =============================================================================
// devproc.thread_files_name_their_thread
// =============================================================================

void test_devproc_thread_files_name_their_thread(void) {
    struct Proc *caller = current_thread()->proc;
    struct dt_fix f;
    TEST_ASSERT(dt_fix_up(&f, caller->principal_id), "the stopped target and its kstacks");

    // (a) The thread directory lists A and B, never the exited X, in runs one
    //     entry wide.
    static struct dt_ent ents[8];
    bool inc = false;
    struct Spoor *td = dt_open(f.pid, -1, NULL, 0);
    int nlist = td ? dt_list(td, 40, ents, 8, &inc) : -2;
    if (td) spoor_clunk(td);
    char sa[12], sb[12];
    dt_udec(sa, (u64)f.ta);
    dt_udec(sb, (u64)f.tb);
    bool listed = nlist == 2 && dt_streq(ents[0].name, sa) && dt_streq(ents[1].name, sb) &&
                  ents[0].qtype == QTDIR && ents[1].qtype == QTDIR &&
                  ents[0].qvers == (u32)f.pid && ents[1].qvers == (u32)f.pid &&
                  ents[0].cookie == (u64)f.ta + 1 && ents[1].cookie == (u64)f.tb + 1;

    // (b) Each thread's regs read its own frame; the Proc-level file reads main.
    struct Spoor *ra = dt_open(f.pid, f.ta, "regs", 2);
    struct Spoor *rb = dt_open(f.pid, f.tb, "regs", 2);
    struct Spoor *rp = dt_open_pidfile(f.pid, "regs", 2);
    u64 pc_a = 0, pc_b = 0, pc_p = 0;
    long rc_a = dt_read_pc(ra, &pc_a);
    long rc_b = dt_read_pc(rb, &pc_b);
    long rc_p = dt_read_pc(rp, &pc_p);

    // (c) A write through B's file lands in B's frame only.
    struct t_user_regs wr;
    for (size_t i = 0; i < sizeof(wr); i++) ((u8 *)&wr)[i] = 0;
    wr.pc = 0xB0B00000ull;
    long wc_b = rb ? devproc.write(rb, &wr, (long)sizeof(wr), 0) : -2;
    u64 elr_a = f.fa->elr, elr_b = f.fb->elr;

    // (d) kregs and fpregs name their thread too.
    struct t_kernel_regs ka, kb;
    struct t_user_fpregs fpa, fpb;
    for (size_t i = 0; i < sizeof(ka); i++) { ((u8 *)&ka)[i] = 0; ((u8 *)&kb)[i] = 0; }
    for (size_t i = 0; i < sizeof(fpa); i++) { ((u8 *)&fpa)[i] = 0; ((u8 *)&fpb)[i] = 0; }
    struct Spoor *kra = dt_open(f.pid, f.ta, "kregs", 0);
    struct Spoor *krb = dt_open(f.pid, f.tb, "kregs", 0);
    struct Spoor *fra = dt_open(f.pid, f.ta, "fpregs", 0);
    struct Spoor *frb = dt_open(f.pid, f.tb, "fpregs", 0);
    long kc_a = kra ? devproc.read(kra, &ka, (long)sizeof(ka), 0) : -2;
    long kc_b = krb ? devproc.read(krb, &kb, (long)sizeof(kb), 0) : -2;
    long fc_a = fra ? devproc.read(fra, &fpa, (long)sizeof(fpa), 0) : -2;
    long fc_b = frb ? devproc.read(frb, &fpb, (long)sizeof(fpb), 0) : -2;
    if (kra) spoor_clunk(kra);
    if (krb) spoor_clunk(krb);
    if (fra) spoor_clunk(fra);
    if (frb) spoor_clunk(frb);

    // (e) The exited thread, and a tid the Proc never had, have no directory.
    struct Spoor *wx = dt_open(f.pid, f.tx, NULL, -1);
    struct Spoor *wn = dt_open(f.pid, f.pid + 6, NULL, -1);
    bool x_absent = wx == NULL, n_absent = wn == NULL;
    if (wx) spoor_unref(wx);
    if (wn) spoor_unref(wn);

    // (f) B leaves the Proc: its open file reads as not found and stats as gone,
    //     and the listing drops it; A's file still reads.
    struct t_stat st;
    int stat_before = rb ? devproc.stat_native(rb, &st) : -2;
    irq_state_t ts = proc_threads_lock(f.tgt);
    g_dt_x.next_in_proc = &g_dt_a;
    proc_threads_unlock(f.tgt, ts);
    u64 pc_gone = 0, pc_a2 = 0;
    long rc_gone = dt_read_pc(rb, &pc_gone);
    long rc_a2   = dt_read_pc(ra, &pc_a2);
    int stat_after = rb ? devproc.stat_native(rb, &st) : -2;
    td = dt_open(f.pid, -1, NULL, 0);
    int nlist2 = td ? dt_list(td, 256, ents, 8, &inc) : -2;
    if (td) spoor_clunk(td);
    bool relisted = nlist2 == 1 && dt_streq(ents[0].name, sa);

    if (ra) spoor_clunk(ra);
    if (rb) spoor_clunk(rb);
    if (rp) spoor_clunk(rp);
    dt_fix_down(&f);

    TEST_ASSERT(listed, "the thread directory lists main and the newer thread, by tid, in order");
    TEST_ASSERT(inc, "every listing cookie exceeds the one before it");
    TEST_EXPECT_EQ(rc_a, (long)sizeof(struct t_user_regs), "main's regs read");
    TEST_EXPECT_EQ(rc_b, (long)sizeof(struct t_user_regs), "the newer thread's regs read");
    TEST_EXPECT_EQ(pc_a, 0xA0000ull, "main's file reads main's frame");
    TEST_EXPECT_EQ(pc_b, 0xB0000ull, "the newer thread's file reads its own frame");
    TEST_EXPECT_EQ(rc_p, (long)sizeof(struct t_user_regs), "the Proc-level regs read");
    TEST_EXPECT_EQ(pc_p, 0xA0000ull, "the Proc-level regs still read main (no focus)");
    TEST_EXPECT_EQ(wc_b, (long)sizeof(wr), "a write through the newer thread's regs lands");
    TEST_EXPECT_EQ(elr_b, 0xB0B00000ull, "the write went to the newer thread's frame");
    TEST_EXPECT_EQ(elr_a, 0xA0000ull, "main's frame is untouched");
    TEST_EXPECT_EQ(kc_a, (long)sizeof(ka), "main's kregs read");
    TEST_EXPECT_EQ(kc_b, (long)sizeof(kb), "the newer thread's kregs read");
    TEST_EXPECT_EQ(ka.tpidr_el0, 0xA00ull, "main's kregs carry main's TLS base");
    TEST_EXPECT_EQ(kb.tpidr_el0, 0xB00ull, "the newer thread's kregs carry its TLS base");
    TEST_EXPECT_EQ(fc_a, (long)sizeof(fpa), "main's fpregs read");
    TEST_EXPECT_EQ(fc_b, (long)sizeof(fpb), "the newer thread's fpregs read");
    TEST_EXPECT_EQ((u64)fpa.fpsr, 0xAull, "main's fpregs are main's");
    TEST_EXPECT_EQ((u64)fpb.fpsr, 0xBull, "the newer thread's fpregs are its own");
    TEST_ASSERT(x_absent, "an exited thread has no directory");
    TEST_ASSERT(n_absent, "a tid the Proc never had has no directory");
    TEST_EXPECT_EQ(stat_before, 0, "the newer thread's file stats while it is the Proc's");
    TEST_EXPECT_EQ(rc_gone, (long)-1, "once it has left, its open file reads as not found");
    TEST_EXPECT_EQ(stat_after, -1, "and stats as gone");
    TEST_EXPECT_EQ(rc_a2, (long)sizeof(struct t_user_regs), "main's file still reads");
    TEST_EXPECT_EQ(pc_a2, 0xA0000ull, "and still reads main's frame");
    TEST_ASSERT(relisted, "the listing drops the departed thread");
}

// =============================================================================
// devproc.thread_step_names_its_thread
// =============================================================================

struct dt_parse_case {
    const char *s;
    bool        ok;
    int         tid;
};

void test_devproc_thread_step_names_its_thread(void) {
    // The operand: absent means the focus (-1); a decimal under the walk's rules
    // names a thread; anything else refuses.
    static const struct dt_parse_case cases[] = {
        { "step",             true,  -1 },
        { "step\n",           true,  -1 },
        { "step 7",           true,   7 },
        { "step 7\n",         true,   7 },
        { "  step\t 12 ",     true,  12 },
        { "step 0",           true,   0 },
        { "step 2147483647",  true,  0x7fffffff },
        { "step 2147483648",  false, 0 },
        { "step 05",          false, 0 },
        { "step x",           false, 0 },
        { "step 7 8",         false, 0 },
        { "step -1",          false, 0 },
    };
    int parse_bad = -1;
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        long n = 0;
        while (cases[i].s[n]) n++;
        int tid = 12345;
        bool ok = devproc_parse_step_tid_for_test(cases[i].s, n, &tid);
        if (ok != cases[i].ok || (ok && tid != cases[i].tid)) { parse_bad = i; break; }
    }

    struct Proc *caller = current_thread()->proc;
    struct dt_fix f;
    TEST_ASSERT(dt_fix_up(&f, caller->principal_id), "the stopped target and its kstacks");
    struct Spoor *ctl = dt_open_pidfile(f.pid, "ctl", 1);
    struct Spoor *other = devproc.attach("");
    f.tgt->debug_owner = ctl;

    // Each arm runs against a stopped target with nothing armed.
    int r_b = devproc_step_arm_for_test(f.pid, f.tb, caller, ctl);
    bool b_armed = g_dt_b.debug_ss_armed, a_by_b = g_dt_a.debug_ss_armed;
    g_dt_a.debug_ss_armed = g_dt_b.debug_ss_armed = false;
    f.tgt->debug_stop_req = 1;

    int r_main = devproc_step_arm_for_test(f.pid, -1, caller, ctl);
    bool a_armed = g_dt_a.debug_ss_armed, b_by_main = g_dt_b.debug_ss_armed;
    g_dt_a.debug_ss_armed = g_dt_b.debug_ss_armed = false;
    f.tgt->debug_stop_req = 1;

    int r_x    = devproc_step_arm_for_test(f.pid, f.tx, caller, ctl);
    int r_none = devproc_step_arm_for_test(f.pid, f.pid + 6, caller, ctl);
    bool none_armed = !g_dt_a.debug_ss_armed && !g_dt_b.debug_ss_armed && !g_dt_x.debug_ss_armed;
    // The slot is still the step's first condition: the same tid from a file
    // that does not hold it refuses.
    int r_owner = devproc_step_arm_for_test(f.pid, f.tb, caller, other);

    f.tgt->debug_owner = NULL;
    if (ctl) spoor_clunk(ctl);
    if (other) spoor_unref(other);
    dt_fix_down(&f);

    TEST_EXPECT_EQ(parse_bad, -1, "every step operand parses as specified (index of the first miss)");
    TEST_ASSERT(ctl != NULL, "the target's ctl opened");
    TEST_EXPECT_EQ(r_b, 1, "step <tid> on the newer thread armed and resumed");
    TEST_ASSERT(b_armed, "the newer thread is armed");
    TEST_ASSERT(!a_by_b, "main is not");
    TEST_EXPECT_EQ(r_main, 1, "a bare step armed and resumed");
    TEST_ASSERT(a_armed, "a bare step arms main (no focus)");
    TEST_ASSERT(!b_by_main, "and not the newer thread");
    TEST_EXPECT_EQ(r_x, -1, "step <tid> of an exited thread refuses");
    TEST_EXPECT_EQ(r_none, -1, "step <tid> of a tid the Proc never had refuses");
    TEST_ASSERT(none_armed, "neither refusal armed a thread");
    TEST_EXPECT_EQ(r_owner, -1, "step <tid> from a file without the slot refuses");
}

// =============================================================================
// devproc.thread_dir_gated
// =============================================================================

struct dt_gate_arg {
    int    pid, tid;
    u32    principal;
    caps_t caps;
};

static volatile long g_dtg_list;
static volatile int  g_dtg_dir, g_dtg_tid, g_dtg_root_n, g_dtg_root_self, g_dtg_root_tgt;

// Runs as a Proc of its own, with the principal and capabilities under test.
static void dt_gate_child(void *arg) {
    const struct dt_gate_arg *a = (const struct dt_gate_arg *)arg;
    struct Proc *self = current_thread()->proc;
    __atomic_store_n(&self->principal_id, a->principal, __ATOMIC_RELEASE);
    __atomic_store_n(&self->caps, a->caps, __ATOMIC_RELEASE);

    struct Spoor *td = dt_open(a->pid, -1, NULL, 0);
    g_dtg_dir = td != NULL;
    u8 buf[256];
    g_dtg_list = td ? devproc.readdir(td, buf, (long)sizeof(buf), 0) : -999;
    if (td) spoor_clunk(td);
    struct Spoor *tid = dt_open(a->pid, a->tid, NULL, -1);
    g_dtg_tid = tid != NULL;
    if (tid) spoor_unref(tid);

    static struct dt_ent ents[64];
    bool inc = false;
    struct Spoor *root = devproc.attach("");
    int n = root ? dt_list(root, 256, ents, 64, &inc) : -2;
    if (root) spoor_unref(root);
    char self_s[12], tgt_s[12];
    dt_udec(self_s, (u64)self->pid);
    dt_udec(tgt_s, (u64)a->pid);
    int seen_self = 0, seen_tgt = 0;
    for (int i = 0; i < n && i < 64; i++) {
        if (dt_streq(ents[i].name, self_s)) seen_self++;
        if (dt_streq(ents[i].name, tgt_s))  seen_tgt++;
    }
    g_dtg_root_n    = n;
    g_dtg_root_self = seen_self;
    g_dtg_root_tgt  = seen_tgt;
    exits("ok");
}

static void dt_gate_run(const struct dt_gate_arg *a) {
    g_dtg_list = -1000;
    g_dtg_dir = g_dtg_tid = -1;
    g_dtg_root_n = g_dtg_root_self = g_dtg_root_tgt = -1;
    int pid = rfork(RFPROC, dt_gate_child, (void *)a);
    if (pid > 0) {
        int st = -1;
        (void)wait_pid(&st);
    }
}

void test_devproc_thread_dir_gated(void) {
    struct dt_fix f;
    TEST_ASSERT(dt_fix_up(&f, 0xA11CEu), "the target and its kstacks");

    struct dt_gate_arg a = { .pid = f.pid, .tid = f.ta };
    a.principal = 0xB0Bu;  a.caps = 0;
    dt_gate_run(&a);
    long s_list = g_dtg_list;
    int  s_dir = g_dtg_dir, s_tid = g_dtg_tid, s_root_tgt = g_dtg_root_tgt;

    a.principal = 0xA11CEu;  a.caps = 0;
    dt_gate_run(&a);
    long o_list = g_dtg_list;
    int  o_tid = g_dtg_tid;

    a.principal = 0xB0Bu;  a.caps = CAP_DEBUG;
    dt_gate_run(&a);
    long d_list = g_dtg_list;
    int  d_tid = g_dtg_tid;

    a.principal = PRINCIPAL_NONE;  a.caps = 0;
    dt_gate_run(&a);
    int n_root = g_dtg_root_n, n_self = g_dtg_root_self, n_tgt = g_dtg_root_tgt;

    dt_fix_down(&f);

    TEST_EXPECT_EQ(s_dir, 1, "a stranger walks to the thread directory itself");
    TEST_EXPECT_EQ(s_list, (long)-T_E_ACCES, "a stranger cannot list the threads (EACCES)");
    TEST_EXPECT_EQ(s_tid, 0, "a stranger cannot walk to a tid");
    TEST_ASSERT(o_list > 0, "the owner lists the threads");
    TEST_EXPECT_EQ(o_tid, 1, "the owner walks to a tid");
    TEST_ASSERT(d_list > 0, "a CAP_DEBUG holder lists another principal's threads");
    TEST_EXPECT_EQ(d_tid, 1, "a CAP_DEBUG holder walks to a tid");
    TEST_EXPECT_EQ(s_root_tgt, 1, "a stranger's root listing shows the target's pid");
    TEST_EXPECT_EQ(n_root, 1, "a caller running as none lists one pid at the root");
    TEST_EXPECT_EQ(n_self, 1, "its own");
    TEST_EXPECT_EQ(n_tgt, 0, "and not the target's");
}

// =============================================================================
// devproc.readdir_lists_every_dir
// =============================================================================

#define DT_EXTRA 20   // more Procs than one listing pass collects

void test_devproc_readdir_lists_every_dir(void) {
    struct Proc *caller = current_thread()->proc;
    struct dt_fix f;
    TEST_ASSERT(dt_fix_up(&f, caller->principal_id), "the target and its kstacks");
    static struct Proc *extra[DT_EXTRA];
    int nextra = 0;
    for (int i = 0; i < DT_EXTRA; i++) {
        extra[i] = proc_alloc();
        if (!extra[i]) break;
        extra[i]->state = PROC_STATE_ALIVE;
        proc_test_link(extra[i]);
        nextra++;
    }

    // (a) The root, a byte run too small for two entries at a time: every extra
    //     Proc, the target and kproc appear once, ascending.
    static struct dt_ent ents[512];
    bool root_inc = false;
    struct Spoor *root = devproc.attach("");
    int nroot = root ? dt_list(root, 40, ents, 512, &root_inc) : -2;
    int root_hits = 0, root_kproc = 0, root_tgt = 0;
    char nm[12];
    for (int i = 0; i < nroot && i < 512; i++) {
        for (int j = 0; j < nextra; j++) {
            dt_udec(nm, (u64)extra[j]->pid);
            if (dt_streq(ents[i].name, nm)) root_hits++;
        }
        if (dt_streq(ents[i].name, "0")) root_kproc++;
        dt_udec(nm, (u64)f.pid);
        if (dt_streq(ents[i].name, nm)) root_tgt++;
    }
    long tiny = root ? devproc.readdir(root, ents, 10, 0) : -2;   // no entry fits in 10 bytes

    // (a2) The root again in runs large enough that one call carries on past a
    //      pass's batch, the continuation the one-entry runs above never reach.
    bool big_inc = false;
    int nbig = root ? dt_list(root, 1024, ents, 512, &big_inc) : -2;
    int big_run = g_dt_list_max_run;
    int big_hits = 0, big_kproc = 0, big_tgt = 0;
    for (int i = 0; i < nbig && i < 512; i++) {
        for (int j = 0; j < nextra; j++) {
            dt_udec(nm, (u64)extra[j]->pid);
            if (dt_streq(ents[i].name, nm)) big_hits++;
        }
        if (dt_streq(ents[i].name, "0")) big_kproc++;
        dt_udec(nm, (u64)f.pid);
        if (dt_streq(ents[i].name, nm)) big_tgt++;
    }
    if (root) spoor_unref(root);

    // (b) The pid directory: its files in table order, then thread/.
    static const char *const pid_files[] = {
        "status", "cmdline", "ctl", "ns", "mem", "regs", "fpregs", "wait", "kregs",
        "kstack", "exe", "cwd", "maps", "sched", "environ", "imperium", "thread",
    };
    const int npf = (int)(sizeof(pid_files) / sizeof(pid_files[0]));
    bool pid_inc = false;
    char ps[12];
    dt_udec(ps, (u64)f.pid);
    const char *pn[1] = { ps };
    struct Spoor *pd = dt_walk(pn, 1);
    int npid = pd ? dt_list(pd, 40, ents, 512, &pid_inc) : -2;
    bool pid_ok = npid == npf;
    for (int i = 0; pid_ok && i < npf; i++) {
        pid_ok = dt_streq(ents[i].name, pid_files[i]) && ents[i].cookie == (u64)i + 1 &&
                 ents[i].qtype == (i == npf - 1 ? QTDIR : QTFILE);
    }
    long past = pd ? devproc.readdir(pd, ents, 256, (s64)npf) : -2;
    if (pd) spoor_unref(pd);

    // (c) A tid directory: its four files, carrying the tid and the pid.
    static const char *const tid_files[] = { "regs", "fpregs", "kregs", "kstack" };
    bool tid_inc = false;
    struct Spoor *tdir = dt_open(f.pid, f.tb, NULL, -1);
    int ntid = tdir ? dt_list(tdir, 40, ents, 512, &tid_inc) : -2;
    bool tid_ok = ntid == 4;
    for (int i = 0; tid_ok && i < 4; i++) {
        tid_ok = dt_streq(ents[i].name, tid_files[i]) && ents[i].qtype == QTFILE &&
                 ents[i].qvers == (u32)f.pid && (u32)(ents[i].qpath >> 32) == (u32)f.tb &&
                 (ents[i].qpath & 0x80000000ull) != 0;
    }
    struct t_stat st_dir;
    int stat_dir = tdir ? devproc.stat_native(tdir, &st_dir) : -2;

    // (d) ".." climbs one level from each kind.
    struct Spoor *file = tdir ? dt_step(tdir, "regs") : NULL;
    struct Spoor *up1 = file ? dt_step(file, "..") : NULL;     // -> the tid directory
    struct Spoor *up2 = up1 ? dt_step(up1, "..") : NULL;       // -> thread/
    struct Spoor *up3 = up2 ? dt_step(up2, "..") : NULL;       // -> the pid directory
    struct Spoor *up4 = up3 ? dt_step(up3, "..") : NULL;       // -> the root
    struct Spoor *stf = up3 ? dt_step(up3, "status") : NULL;
    struct Spoor *up5 = stf ? dt_step(stf, "..") : NULL;       // a pid file -> its directory
    bool climbs = up1 && tdir && up1->qid.path == tdir->qid.path && up1->qid.vers == (u32)f.pid &&
                  up2 && up2->qid.type == QTDIR && (up2->qid.path >> 32) == (u64)(u32)f.pid &&
                  up3 && up4 && up4->qid.path == 0 &&
                  up5 && up5->qid.path == up3->qid.path;
    struct t_stat st_file;
    int stat_file = file ? devproc.stat_native(file, &st_file) : -2;
    long file_rd = file ? devproc.readdir(file, ents, 256, 0) : -2;
    struct Spoor *spoors[] = { file, up1, up2, up3, up4, stf, up5, tdir };
    for (int i = 0; i < (int)(sizeof(spoors) / sizeof(spoors[0])); i++)
        if (spoors[i]) spoor_unref(spoors[i]);

    for (int i = 0; i < nextra; i++) {
        proc_test_unlink(extra[i]);
        extra[i]->state = PROC_STATE_ZOMBIE;
        proc_free(extra[i]);
    }
    dt_fix_down(&f);

    TEST_EXPECT_EQ(nextra, DT_EXTRA, "the extra Procs were made");
    TEST_ASSERT(nroot > DT_EXTRA, "the root listed more entries than one pass collects");
    TEST_ASSERT(root_inc, "the root's cookies only increase");
    TEST_EXPECT_EQ(root_hits, nextra, "every extra Proc is listed exactly once");
    TEST_EXPECT_EQ(root_kproc, 1, "kproc is listed once");
    TEST_EXPECT_EQ(root_tgt, 1, "the target is listed once");
    TEST_EXPECT_EQ(tiny, (long)-1, "a buffer no entry fits is refused");
    TEST_ASSERT(big_run > 16, "one large run carried on past a pass's batch of 16");
    TEST_ASSERT(big_inc, "the large runs' cookies only increase");
    TEST_EXPECT_EQ(big_hits, nextra, "in large runs every extra Proc is listed exactly once");
    TEST_EXPECT_EQ(big_kproc, 1, "in large runs kproc is listed once");
    TEST_EXPECT_EQ(big_tgt, 1, "in large runs the target is listed once");
    TEST_ASSERT(pid_ok, "the pid directory lists its files in order, then thread/");
    TEST_ASSERT(pid_inc, "the pid directory's cookies only increase");
    TEST_EXPECT_EQ(past, (long)0, "a cookie past the last entry ends the listing");
    TEST_ASSERT(tid_ok, "the tid directory lists its four files, each naming the tid and the pid");
    TEST_ASSERT(tid_inc, "the tid directory's cookies only increase");
    TEST_EXPECT_EQ(stat_dir, 0, "the tid directory stats");
    TEST_EXPECT_EQ(st_dir.mode, (u32)(T_S_IFDIR | 0555u), "as a directory, 0555");
    TEST_EXPECT_EQ(stat_file, 0, "a tid file stats");
    TEST_EXPECT_EQ(st_file.mode, (u32)(T_S_IFREG | 0600u), "regs as a file, 0600");
    TEST_ASSERT(climbs, "\"..\" climbs one level from a tid file, a tid directory, thread/, a pid directory and a pid file");
    TEST_EXPECT_EQ(file_rd, (long)-1, "a file does not list");
}
