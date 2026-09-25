// /proc Dev tests (P4-C).
//
// Per ROADMAP §6.1 + §6.2. Tests cover:
//
//   devproc.bestiary_smoke           — devproc registered; lookup by dc/name
//   devproc.attach_returns_dir       — attach yields QTDIR root
//   devproc.walk_root_to_kproc_dir   — walk("0") from root → /proc/0/ QTDIR
//   devproc.walk_unknown_pid_misses  — walk("99999") returns nqid=0
//   devproc.walk_to_status_file      — walk path /proc/0/status reaches QTFILE
//   devproc.walk_dotdot_to_root      — walk("..") from any node → root
//   devproc.read_status_format       — read /proc/0/status; verify text
//   devproc.read_cmdline_kproc       — read /proc/0/cmdline; "kproc"
//   devproc.read_ns_format           — read /proc/0/ns; "binds: 0"
//   devproc.read_ctl_returns_zero    — ctl reads return 0
//   devproc.write_ctl_consumes       — ctl writes return n; non-ctl writes -1
//   devproc.read_dir_returns_neg1    — reading a directory qid returns -1
//                                       (readdir lands later)
//   devproc.read_partial_offset      — offset-aware read returns the right
//                                       slice; off >= len returns 0 (EOF)

#include "test.h"

#include "../../arch/arm64/mmu.h"        // 8a-1b-gamma-1: mmu_install_user_pte + mmu_cross_proc_* + pa_to_kva
#include "../../arch/arm64/exception.h"  // 8a-1b-gamma-2: struct exception_context (the regs test's synthetic trapframe)
#include "../../mm/phys.h"               // alloc_pages / free_pages

#include <thylacine/burrow.h>          // V-4b-2: burrow_create_anon/map -- /proc/<pid>/maps
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/env.h>             // V-4b-6: env_create/write -- /proc/<pid>/environ
#include <thylacine/exec.h>            // V-4b-2: EXEC_USER_STACK_BASE -- the maps role tag
#include <thylacine/page.h>
#include <thylacine/path.h>            // V-4a-0: /proc/<pid>/exe
#include <thylacine/proc.h>
#include <thylacine/sched.h>       // prowl-1/3a: sched() + sched_cpu_idle_ns
#include <thylacine/smp.h>         // prowl-3a: smp_cpu_count() -- last_cpu bound
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>     // V-4b-1: a fresh Territory for the cwd target
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_devproc_bestiary_smoke(void);
void test_devproc_attach_returns_dir(void);
void test_devproc_walk_root_to_kproc_dir(void);
void test_devproc_walk_unknown_pid_misses(void);
void test_devproc_walk_to_status_file(void);
void test_devproc_walk_dotdot_to_root(void);
void test_devproc_read_status_format(void);
void test_devproc_read_cmdline_kproc(void);
void test_devproc_read_ns_format(void);
void test_devproc_read_ctl_returns_zero(void);
void test_devproc_write_ctl_rejects(void);
void test_devproc_read_dir_returns_neg1(void);
void test_devproc_read_partial_offset(void);
// A-4b: cross-process kill via /proc/<pid>/ctl.
void test_devproc_kill_authorized_predicate(void);
void test_devproc_stat_native_ctl_owner(void);
void test_devproc_write_ctl_kill_dispatch(void);
void test_devproc_ctl_suspend_resume_dispatch(void);   // prowl-4: job-control stop/cont verb
// 8a-1b: the I-39 debug gate + the attach/detach/close slot lifecycle.
void test_devproc_debug_authorized_predicate(void);
void test_devproc_debug_cap_cover_predicate(void);
void test_devproc_debug_cap_cover_attach(void);
void test_devproc_debug_attach_detach_lifecycle(void);
void test_devproc_debug_stop_start_resume(void);
void test_devproc_debug_mem(void);
void test_devproc_debug_regs(void);
void test_devproc_debug_kregs_kstack_wait(void);
void test_devproc_debug_kstack_settled(void);
void test_devproc_debug_step_cancel_on_stop(void);
void test_devproc_read_exe(void);            // VIVARIUM V-4a-0
void test_devproc_read_cwd(void);            // VIVARIUM V-4b-1
void test_devproc_maps(void);                // VIVARIUM V-4b-2
void test_devproc_environ(void);             // VIVARIUM V-4b-6
// prowl-3b: /proc/<pid>/sched read + the OQ-4 owner-or-CAP_HOSTOWNER gate.
void test_devproc_sched_gate_predicate(void);
void test_devproc_sched_read_gated(void);
void test_devproc_read_sched_format(void);
// #133: the settled-park decision (a stale park must not read as stopped).
void test_devproc_park_state_settled(void);
// IM-2: /proc/<pid>/imperium (the legate scope; owner-or-CAP_HOSTOWNER).
void test_devproc_imperium_read_gated(void);
void test_devproc_read_imperium_format(void);

// A-4b + 8a-1b impl hooks (non-static in kernel/devproc.c) + Proc test helpers
// (non-static in kernel/proc.c; the test_proc.c / test_devsrv_conn.c pattern).
bool devproc_kill_authorized(const struct Proc *caller, const struct Proc *target);
bool devproc_debug_authorized(const struct Proc *caller, const struct Proc *target);
bool devproc_sched_authorized(const struct Proc *caller, const struct Proc *target);
bool devproc_owner_or_hostowner(const struct Proc *caller, const struct Proc *target);
bool devproc_extract_authorized(const struct Proc *caller, const struct Proc *target);
size_t devproc_sched_read_gated(const struct Proc *caller, struct Proc *target,
                                char *buf, size_t cap, bool *denied);
size_t devproc_imperium_read_gated(const struct Proc *caller, struct Proc *target,
                                   char *buf, size_t cap, bool *denied);   // IM-2
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
bool devproc_park_state_is_settled(bool registered, bool on_cpu, int state);
bool devproc_all_threads_parked(struct Proc *target);

// =============================================================================
// Helpers.
// =============================================================================

// Quick string contains. Returns true if needle is a substring of haystack.
static bool contains(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0) return true;
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return true;
    }
    return false;
}

// V-4b-2: byte offset of the first occurrence of `needle`, or -1. Distinct from
// contains() because an ordering check needs the POSITION, not the presence --
// scanning with contains() matches at every index and yields 0 for everything.
static long index_of(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0 || nlen > hlen) return -1;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) j++;
        if (j == nlen) return (long)i;
    }
    return -1;
}

// prowl-1: exact NUL-terminated string equality (proc_set_name basename check).
static bool streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

// prowl-1: read kproc's cumulative cpu_ns under g_proc_table_lock (proc_for_each
// holds it -- proc_cpu_ns's precondition). Stashes it via the void* arg.
static int cpu_ns_cb(struct Proc *p, void *arg) {
    if (p == kproc()) *(u64 *)arg = proc_cpu_ns(p);
    return 0;
}

// Walk one component from c using devproc->walk; return the new Spoor or
// NULL on miss / failure. Frees the Walkqid for the caller. On miss
// (nqid == 0), spoor_unref's the result Spoor.
static struct Spoor *walk_one(struct Spoor *c, const char *name) {
    const char *names[1] = { name };
    struct Walkqid *wq = devproc.walk(c, NULL, names, 1);
    if (!wq) return NULL;
    if (wq->nqid != 1) {
        // miss
        spoor_unref(wq->spoor);
        walkqid_free(wq);
        return NULL;
    }
    struct Spoor *result = wq->spoor;
    walkqid_free(wq);
    return result;
}

// Open via attach + walk + open. Caller spoor_clunk's the result.
static struct Spoor *open_status_for_pid(int pid) {
    struct Spoor *root = devproc.attach("");
    if (!root) return NULL;

    char pidstr[12];
    int n = 0;
    int v = pid;
    if (v == 0) pidstr[n++] = '0';
    else {
        char tmp[12]; int tn = 0;
        while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
        for (int i = tn - 1; i >= 0; i--) pidstr[n++] = tmp[i];
    }
    pidstr[n] = '\0';

    struct Spoor *piddir = walk_one(root, pidstr);
    spoor_unref(root);
    if (!piddir) return NULL;
    struct Spoor *status = walk_one(piddir, "status");
    spoor_unref(piddir);
    if (!status) return NULL;
    if (!devproc.open(status, 0)) {
        spoor_unref(status);
        return NULL;
    }
    return status;
}

// Open /proc/<pid>/ctl for write (OWRITE = 1). Caller spoor_clunk's the
// result. Used by the A-4b kill tests.
static struct Spoor *open_ctl_for_pid(int pid) {
    struct Spoor *root = devproc.attach("");
    if (!root) return NULL;

    char pidstr[12];
    int n = 0;
    int v = pid;
    if (v == 0) pidstr[n++] = '0';
    else {
        char tmp[12]; int tn = 0;
        while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
        for (int i = tn - 1; i >= 0; i--) pidstr[n++] = tmp[i];
    }
    pidstr[n] = '\0';

    struct Spoor *piddir = walk_one(root, pidstr);
    spoor_unref(root);
    if (!piddir) return NULL;
    struct Spoor *ctl = walk_one(piddir, "ctl");
    spoor_unref(piddir);
    if (!ctl) return NULL;
    if (!devproc.open(ctl, 1)) {        // OWRITE
        spoor_unref(ctl);
        return NULL;
    }
    return ctl;
}

// 8a-1b-gamma: open /proc/<pid>/mem for read+write (ORDWR = 2). Caller
// spoor_clunk's the result.
static struct Spoor *open_mem_for_pid(int pid) {
    struct Spoor *root = devproc.attach("");
    if (!root) return NULL;
    char pidstr[12]; int n = 0; int v = pid;
    if (v == 0) pidstr[n++] = '0';
    else { char tmp[12]; int tn = 0;
           while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
           for (int i = tn - 1; i >= 0; i--) pidstr[n++] = tmp[i]; }
    pidstr[n] = '\0';
    struct Spoor *piddir = walk_one(root, pidstr);
    spoor_unref(root);
    if (!piddir) return NULL;
    struct Spoor *mem = walk_one(piddir, "mem");
    spoor_unref(piddir);
    if (!mem) return NULL;
    if (!devproc.open(mem, 2)) {        // ORDWR
        spoor_unref(mem);
        return NULL;
    }
    return mem;
}

// 8a-1b-gamma-2: open /proc/<pid>/<name> with `omode`. Caller spoor_clunk's it.
static struct Spoor *open_pidfile_for(int pid, const char *name, int omode) {
    struct Spoor *root = devproc.attach("");
    if (!root) return NULL;
    char pidstr[12]; int n = 0; int v = pid;
    if (v == 0) pidstr[n++] = '0';
    else { char tmp[12]; int tn = 0;
           while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
           for (int i = tn - 1; i >= 0; i--) pidstr[n++] = tmp[i]; }
    pidstr[n] = '\0';
    struct Spoor *piddir = walk_one(root, pidstr);
    spoor_unref(root);
    if (!piddir) return NULL;
    struct Spoor *f = walk_one(piddir, name);
    spoor_unref(piddir);
    if (!f) return NULL;
    if (!devproc.open(f, omode)) { spoor_unref(f); return NULL; }
    return f;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devproc_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('p'),       &devproc, "lookup 'p' = devproc");
    TEST_EXPECT_EQ(dev_lookup_by_name("proc"),  &devproc, "lookup 'proc' = devproc");
    TEST_EXPECT_EQ(devproc.dc, 'p',                       "devproc.dc = 'p'");
}

void test_devproc_attach_returns_dir(void) {
    struct Spoor *c = devproc.attach("");
    TEST_ASSERT(c != NULL, "devproc.attach succeeds");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root qid.type = QTDIR");
    TEST_EXPECT_EQ(c->dev, &devproc, "back-pointer correct");

    spoor_unref(c);
}

void test_devproc_walk_root_to_kproc_dir(void) {
    struct Spoor *root = devproc.attach("");
    TEST_ASSERT(root != NULL, "attach OK");

    struct Spoor *piddir = walk_one(root, "0");
    spoor_unref(root);
    TEST_ASSERT(piddir != NULL, "walk('0') from root yields the kproc piddir");
    TEST_EXPECT_EQ(piddir->qid.type, QTDIR, "kproc piddir is QTDIR");
    TEST_ASSERT(piddir->qid.path != 0, "piddir qid.path != root path (0)");

    spoor_unref(piddir);
}

void test_devproc_walk_unknown_pid_misses(void) {
    struct Spoor *root = devproc.attach("");
    TEST_ASSERT(root != NULL, "attach OK");

    const char *names[1] = { "99999" };
    struct Walkqid *wq = devproc.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates Walkqid even on miss");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk('99999') misses (nqid=0)");

    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

void test_devproc_walk_to_status_file(void) {
    struct Spoor *root = devproc.attach("");
    TEST_ASSERT(root != NULL, "attach OK");

    // Two-step walk in one call: ["0", "status"] should produce nqid=2.
    const char *names[2] = { "0", "status" };
    struct Walkqid *wq = devproc.walk(root, NULL, names, 2);
    TEST_ASSERT(wq != NULL, "walk allocated");
    TEST_EXPECT_EQ(wq->nqid, 2, "two-step walk succeeds");
    TEST_EXPECT_EQ(wq->spoor->qid.type, QTFILE, "status is QTFILE");

    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

void test_devproc_walk_dotdot_to_root(void) {
    struct Spoor *root = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    TEST_ASSERT(piddir != NULL, "walk to /proc/0/ OK");

    struct Spoor *up = walk_one(piddir, "..");
    TEST_ASSERT(up != NULL, "walk('..') from /proc/0/ succeeds");
    TEST_EXPECT_EQ(up->qid.path, (u64)0, "'..' from /proc/0/ → root path 0");
    TEST_EXPECT_EQ(up->qid.type, QTDIR, "still QTDIR");

    spoor_unref(up);
    spoor_unref(piddir);
    spoor_unref(root);
}

void test_devproc_read_status_format(void) {
    struct Spoor *c = open_status_for_pid(0);
    TEST_ASSERT(c != NULL, "open /proc/0/status OK");

    char buf[512];
    long got = devproc.read(c, buf, 512, 0);
    TEST_ASSERT(got > 0, "read returns positive byte count");

    TEST_ASSERT(contains(buf, (size_t)got, "pid:"),     "status contains 'pid:'");
    TEST_ASSERT(contains(buf, (size_t)got, "0"),        "status contains kproc pid '0'");
    TEST_ASSERT(contains(buf, (size_t)got, "state:"),   "status contains 'state:'");
    TEST_ASSERT(contains(buf, (size_t)got, "ALIVE"),    "kproc state is ALIVE");
    TEST_ASSERT(contains(buf, (size_t)got, "threads:"), "status contains 'threads:'");
    // prowl-1: Plan 9 parity -- name + cpu time + parent + owner.
    TEST_ASSERT(contains(buf, (size_t)got, "name:"),    "status contains 'name:'");
    TEST_ASSERT(contains(buf, (size_t)got, "kproc"),    "kproc status carries its name");
    TEST_ASSERT(contains(buf, (size_t)got, "cpu_ns:"),  "status contains 'cpu_ns:'");
    TEST_ASSERT(contains(buf, (size_t)got, "ppid:"),    "status contains 'ppid:'");
    TEST_ASSERT(contains(buf, (size_t)got, "principal:"), "status contains 'principal:'");

    spoor_clunk(c);
}

// prowl-1 (PROWL-DESIGN.md section 3): the telemetry substrate --
//   (a) proc_set_name extracts the basename correctly, and
//   (b) the ctx-switch run_ns accounting is LIVE: kproc's cpu_ns accrues
//       after forced yields and never decreases.
void test_proc_cpu_ns_accounting(void);
void test_proc_cpu_ns_accounting(void) {
    // (a) proc_set_name basename extraction -- pure, on a stack Proc (only
    // p->name is touched, so no other field need be valid).
    struct Proc tp;
    tp.name[0] = '\0';
    proc_set_name(&tp, "/bin/corvus", 11);
    TEST_ASSERT(streq(tp.name, "corvus"), "basename '/bin/corvus' -> 'corvus'");
    proc_set_name(&tp, "joey", 4);
    TEST_ASSERT(streq(tp.name, "joey"), "no-slash name kept whole");
    proc_set_name(&tp, "/a/b/c/thing", 12);
    TEST_ASSERT(streq(tp.name, "thing"), "deep-path basename -> 'thing'");
    proc_set_name(&tp, "keep", 4);
    proc_set_name(&tp, "/dir/", 5);           // trailing slash -> empty basename
    TEST_ASSERT(streq(tp.name, "keep"), "trailing-slash keeps the prior name");
    // Truncation: a > PROC_NAME_MAX-1 name stays NUL-terminated within bounds.
    proc_set_name(&tp, "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 41);
    TEST_ASSERT(tp.name[PROC_NAME_MAX - 1u] == '\0', "long name stays NUL-terminated");

    // (b) run_ns accounting. sched() yields kthread to its CPU's idle and back;
    // each yield switches kthread OUT so its run_ns accrues (proc_cpu_ns sums
    // over kproc's threads). A measurable busy slice between yields guarantees a
    // nonzero delta even at a coarse CNTVCT resolution.
    for (int i = 0; i < 8; i++) {
        for (volatile int j = 0; j < 1000000; j++) { /* burn a measurable slice */ }
        sched();
    }
    u64 cpu_ns = 0;
    proc_for_each(cpu_ns_cb, &cpu_ns);        // proc_cpu_ns(kproc) under the lock
    TEST_ASSERT(cpu_ns > 0, "kproc cpu_ns accrued after forced yields");

    // Monotonic: another round of run never decreases the cumulative counter.
    for (int i = 0; i < 8; i++) {
        for (volatile int j = 0; j < 1000000; j++) { }
        sched();
    }
    u64 cpu_ns2 = 0;
    proc_for_each(cpu_ns_cb, &cpu_ns2);
    TEST_ASSERT(cpu_ns2 >= cpu_ns, "cpu_ns is monotonic across polls");
}

// prowl-3a: the per-thread scheduler counters (nsched/nsleeps/nmigrations/
// last_cpu) + the per-CPU idle_ns accessor -- the /proc/<pid>/sched + /ctl/cpu
// substrate. Drives the SAME forced-yield vehicle as the run_ns test above: a
// yield switches the running thread OUT then a peer/idle switches it back IN, so
// nsched grows; the burned slice guarantees a real switch rather than a no-op.
void test_sched_prowl_counters(void);
void test_sched_prowl_counters(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL, "current thread present");

    // nsched grows across forced yields (the switch chokepoint bumps it on
    // switch-IN, exactly where run_ns accrues on switch-OUT above).
    u64 n0 = __atomic_load_n(&t->nsched, __ATOMIC_RELAXED);
    for (int i = 0; i < 8; i++) {
        for (volatile int j = 0; j < 500000; j++) { /* burn a measurable slice */ }
        sched();
    }
    u64 n1 = __atomic_load_n(&t->nsched, __ATOMIC_RELAXED);
    TEST_ASSERT(n1 > n0, "nsched grows across forced yields");

    // last_cpu is a valid online CPU index once the thread has been dispatched.
    u16 lc = __atomic_load_n(&t->last_cpu, __ATOMIC_RELAXED);
    TEST_ASSERT((unsigned)lc < smp_cpu_count(), "last_cpu is a valid CPU index");

    // The other counters are monotonic across polls (the diff-across-polls
    // contract the userspace reader depends on -- no wrap, no decrease).
    u64 s0 = __atomic_load_n(&t->nsleeps, __ATOMIC_RELAXED);
    u64 m0 = __atomic_load_n(&t->nmigrations, __ATOMIC_RELAXED);
    for (int i = 0; i < 4; i++) {
        for (volatile int j = 0; j < 200000; j++) { }
        sched();
    }
    TEST_ASSERT(__atomic_load_n(&t->nsleeps, __ATOMIC_RELAXED) >= s0,
                "nsleeps is monotonic across polls");
    TEST_ASSERT(__atomic_load_n(&t->nmigrations, __ATOMIC_RELAXED) >= m0,
                "nmigrations is monotonic across polls");

    // Per-CPU idle_ns: CPU 0 (online) reads a coherent cumulative value without
    // faulting; an out-of-range CPU reads a clean 0 (the accessor's bounds
    // guard), never garbage.
    (void)sched_cpu_idle_ns(0);
    TEST_EXPECT_EQ(sched_cpu_idle_ns(9999u), (u64)0,
                   "out-of-range CPU idle_ns reads 0");
}

void test_devproc_read_cmdline_kproc(void) {
    struct Spoor *root = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *cmdline = walk_one(piddir, "cmdline");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(cmdline != NULL, "walk to /proc/0/cmdline OK");
    TEST_ASSERT(devproc.open(cmdline, 0) != NULL, "open cmdline");

    char buf[64];
    long got = devproc.read(cmdline, buf, 64, 0);
    TEST_ASSERT(got > 0, "cmdline read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "kproc"), "kproc cmdline contains 'kproc'");

    spoor_clunk(cmdline);
}

void test_devproc_read_ns_format(void) {
    struct Spoor *root = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *ns = walk_one(piddir, "ns");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(ns != NULL, "walk to /proc/0/ns OK");
    TEST_ASSERT(devproc.open(ns, 0) != NULL, "open ns");

    char buf[64];
    long got = devproc.read(ns, buf, 64, 0);
    TEST_ASSERT(got > 0, "ns read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "binds:"), "ns contains 'binds:'");

    spoor_clunk(ns);
}

void test_devproc_read_ctl_returns_zero(void) {
    struct Spoor *root = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *ctl = walk_one(piddir, "ctl");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(ctl != NULL, "walk to /proc/0/ctl OK");
    TEST_ASSERT(devproc.open(ctl, 0) != NULL, "open ctl");

    char buf[16];
    long got = devproc.read(ctl, buf, 16, 0);
    TEST_EXPECT_EQ(got, (long)0, "ctl read returns 0 (write-only at v1.0)");

    spoor_clunk(ctl);
}

// A-4b: ctl rejects non-kill verbs, protects kproc, and rejects writes to
// non-ctl files. (The pre-A-4b stub returned n for any ctl write; that is
// gone -- a ctl write now performs the kill verb or fails.)
void test_devproc_write_ctl_rejects(void) {
    // "kill" to /proc/0/ctl (kproc, the kernel proc) is REFUSED -- kproc is
    // unkillable, before any authority check.
    struct Spoor *kctl = open_ctl_for_pid(0);
    TEST_ASSERT(kctl != NULL, "open /proc/0/ctl");
    const char kill_cmd[] = "kill";
    TEST_EXPECT_EQ(devproc.write(kctl, kill_cmd, (long)sizeof(kill_cmd) - 1, 0),
                   (long)-1, "kill of kproc (pid 0) is refused (-1)");
    // An unrecognized verb on the same ctl is also -1 (NOT consumed-as-n).
    const char junk[] = "frobnicate";
    TEST_EXPECT_EQ(devproc.write(kctl, junk, (long)sizeof(junk) - 1, 0),
                   (long)-1, "unknown ctl verb returns -1");
    spoor_clunk(kctl);

    // Writes to non-ctl files (e.g., status) are rejected.
    struct Spoor *status = open_status_for_pid(0);
    TEST_ASSERT(status != NULL, "open status");
    TEST_EXPECT_EQ(devproc.write(status, kill_cmd, (long)sizeof(kill_cmd) - 1, 0),
                   (long)-1, "writes to status return -1");
    spoor_clunk(status);
}

void test_devproc_read_dir_returns_neg1(void) {
    struct Spoor *root = devproc.attach("");
    TEST_ASSERT(devproc.open(root, 0) != NULL, "open root");

    char buf[16];
    long got = devproc.read(root, buf, 16, 0);
    TEST_EXPECT_EQ(got, (long)-1,
                   "directory read returns -1 (readdir not yet implemented)");

    spoor_clunk(root);
}

void test_devproc_read_partial_offset(void) {
    struct Spoor *c = open_status_for_pid(0);
    TEST_ASSERT(c != NULL, "open status");

    char full[256];
    long full_n = devproc.read(c, full, 256, 0);
    TEST_ASSERT(full_n > 0, "full read positive");

    // Read offset 5, max 10 bytes.
    char partial[16];
    long got = devproc.read(c, partial, 10, 5);
    TEST_ASSERT(got > 0, "partial read positive");
    TEST_ASSERT(got <= 10, "partial read bounded by n");

    // Verify partial[0..got] == full[5..5+got].
    for (long i = 0; i < got; i++) {
        TEST_ASSERT(partial[i] == full[5 + i],
                    "partial slice matches the corresponding window of full");
    }

    // Off past EOF returns 0.
    long eof = devproc.read(c, partial, 16, full_n + 100);
    TEST_EXPECT_EQ(eof, (long)0, "off > total returns 0 (EOF)");

    spoor_clunk(c);
}

// =============================================================================
// A-4b: cross-process kill via /proc/<pid>/ctl (IDENTITY-DESIGN.md §9.8, I-26).
// =============================================================================

// The two-axis kill-authority predicate: owner (same principal_id on the 0600
// ctl) OR CAP_HOSTOWNER OR CAP_KILL -- checked DIRECTLY. CAP_DAC_OVERRIDE is
// deliberately NOT a kill axis (the A-4 split keeps fs-admin orthogonal to
// process-kill; mirrors Linux DAC_OVERRIDE vs CAP_KILL).
void test_devproc_kill_authorized_predicate(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");

    target->principal_id = 0xA11CEu;
    target->primary_gid  = 0x6u;

    // 1. Different principal, no caps -> denied.
    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    TEST_ASSERT(!devproc_kill_authorized(caller, target),
                "non-owner with no caps cannot kill");

    // 2. Same principal (owner) -> allowed.
    caller->principal_id = 0xA11CEu;
    TEST_ASSERT(devproc_kill_authorized(caller, target),
                "the owner (same principal) can kill");

    // 3. Different principal + CAP_KILL -> allowed (cross-identity override).
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_KILL;
    TEST_ASSERT(devproc_kill_authorized(caller, target),
                "CAP_KILL authorizes a cross-identity kill");

    // 4. Different principal + CAP_HOSTOWNER -> allowed (unified admin).
    caller->caps = CAP_HOSTOWNER;
    TEST_ASSERT(devproc_kill_authorized(caller, target),
                "CAP_HOSTOWNER authorizes a kill");

    // 5. Different principal + CAP_DAC_OVERRIDE -> DENIED. fs-rwx admin is not
    //    a kill axis (least-privilege; the A-4 split's whole point).
    caller->caps = CAP_DAC_OVERRIDE;
    TEST_ASSERT(!devproc_kill_authorized(caller, target),
                "CAP_DAC_OVERRIDE is NOT a kill axis");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

// prowl-3b: the OQ-4 deep-internals gate for /proc/<pid>/sched -- owner OR
// CAP_HOSTOWNER, and STRICTLY NARROWER than the kill/debug gates (CAP_KILL,
// CAP_DEBUG, CAP_DAC_OVERRIDE are deliberately NOT axes for scheduler telemetry).
void test_devproc_sched_gate_predicate(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");
    target->principal_id = 0xA11CEu;

    // 1. Different principal, no caps -> denied.
    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    TEST_ASSERT(!devproc_sched_authorized(caller, target),
                "non-owner with no caps cannot read the sched view");

    // 2. Same principal (owner) -> allowed.
    caller->principal_id = 0xA11CEu;
    TEST_ASSERT(devproc_sched_authorized(caller, target),
                "the owner (same principal) can read its own sched view");

    // 3. Different principal + CAP_HOSTOWNER -> allowed (the operator).
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_HOSTOWNER;
    TEST_ASSERT(devproc_sched_authorized(caller, target),
                "CAP_HOSTOWNER authorizes the sched view");

    // 4-6. NARROWER than kill/debug: CAP_KILL / CAP_DEBUG / CAP_DAC_OVERRIDE are
    //      NOT sched-view axes (reading telemetry is neither kill nor debug nor
    //      fs-admin -- keep the capability split orthogonal, I-22).
    caller->caps = CAP_KILL;
    TEST_ASSERT(!devproc_sched_authorized(caller, target),
                "CAP_KILL is NOT a sched-view axis");
    caller->caps = CAP_DEBUG;
    TEST_ASSERT(!devproc_sched_authorized(caller, target),
                "CAP_DEBUG is NOT a sched-view axis");
    caller->caps = CAP_DAC_OVERRIDE;
    TEST_ASSERT(!devproc_sched_authorized(caller, target),
                "CAP_DAC_OVERRIDE is NOT a sched-view axis");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

// prowl-3b (prowl-5 F4): the OQ-4 DENY WIRING revert-probe. The predicate test
// above covers the authority logic standalone; the format test below covers only
// the allow leg (kproc-self). This drives the WIRED gate (devproc_sched_read_gated,
// exactly what devproc_read_cb calls) with a synthetic caller, so dropping the
// gate check makes the deny leg fail -- the coverage the real in-unit path
// (kproc/CAP_ALL, always authorized) can never provide.
void test_devproc_sched_read_gated(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");
    target->principal_id = 0xA11CEu;

    char buf[2048];
    bool denied;

    // Non-owner, no caps -> DENIED, zero bytes formatted (no partial leak).
    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    denied = false;
    size_t n = devproc_sched_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(denied && n == 0, "non-owner sched read denied, no bytes formatted");

    // Owner -> allowed; the block formats (at least the name/pid/threads header).
    caller->principal_id = 0xA11CEu;
    denied = true;
    n = devproc_sched_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(!denied && n > 0, "owner sched read allowed, block formatted");

    // CAP_HOSTOWNER (non-owner) -> allowed.
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_HOSTOWNER;
    denied = true;
    n = devproc_sched_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(!denied && n > 0, "CAP_HOSTOWNER sched read allowed");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

// prowl-3b: read /proc/0/sched (kproc reads its OWN sched view -> owner-gated
// allow) and verify the per-thread block rendered -- the column header + the
// proc name. The deny path is the predicate test above (the test runner runs as
// kproc, so it cannot exercise a cross-principal denial here).
void test_devproc_read_sched_format(void) {
    struct Spoor *root   = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *sched  = walk_one(piddir, "sched");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(sched != NULL, "walk to /proc/0/sched OK");
    TEST_ASSERT(devproc.open(sched, 0) != NULL, "open sched");

    char buf[2048];   // == DEVPROC_READ_BUF (devproc.c-private); a full sched read fits
    long got = devproc.read(sched, buf, (long)sizeof(buf), 0);
    TEST_ASSERT(got > 0, "sched read positive (owner-gated allow for kproc-self)");
    TEST_ASSERT(contains(buf, (size_t)got, "name:"), "sched has the proc name");
    TEST_ASSERT(contains(buf, (size_t)got, "tid band cpu"),
                "sched has the per-thread column header");

    spoor_clunk(sched);
}

// stat_native reports the target Proc as the per-pid object's owner, with the
// Plan 9 /proc mode convention (ctl 0600, info files 0444) and the POSIX
// file-type bits (V-4b-5); the dev apex stats as the directory it is (V-4b-4);
// and a zero-padded pid does not name a Proc (V-4b-5).
void test_devproc_stat_native_ctl_owner(void) {
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "proc_alloc target");
    tgt->principal_id = 0x51A7u;
    tgt->primary_gid  = 0x9u;
    tgt->state        = PROC_STATE_ALIVE;
    proc_test_link(tgt);                  // so proc_find_by_pid resolves it

    struct Spoor *root = devproc.attach("");
    TEST_ASSERT(root != NULL, "attach");
    char pidstr[12]; int pn = 0; int v = tgt->pid;
    if (v == 0) pidstr[pn++] = '0';
    else { char tb[12]; int tn = 0; while (v > 0) { tb[tn++] = (char)('0' + v % 10); v /= 10; }
           for (int i = tn - 1; i >= 0; i--) pidstr[pn++] = tb[i]; }
    pidstr[pn] = '\0';
    struct Spoor *piddir = walk_one(root, pidstr);
    spoor_unref(root);
    TEST_ASSERT(piddir != NULL, "walk to piddir");

    struct Spoor *ctl = walk_one(piddir, "ctl");
    TEST_ASSERT(ctl != NULL, "walk to ctl");
    struct t_stat st;
    TEST_EXPECT_EQ(devproc.stat_native(ctl, &st), 0, "stat_native(ctl) ok");
    TEST_EXPECT_EQ(st.uid, tgt->principal_id, "ctl uid = target principal");
    TEST_EXPECT_EQ(st.gid, tgt->primary_gid,  "ctl gid = target primary_gid");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0600u),
                   "ctl mode = S_IFREG|0600 (owner-private)");
    TEST_EXPECT_EQ(st.qid_type, QTFILE,       "ctl is QTFILE");
    spoor_unref(ctl);

    struct Spoor *status = walk_one(piddir, "status");
    TEST_ASSERT(status != NULL, "walk to status");
    TEST_EXPECT_EQ(devproc.stat_native(status, &st), 0, "stat_native(status) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0444u),
                   "status mode = S_IFREG|0444 (world-readable)");
    spoor_unref(status);

    // The FILE-TYPE bits (V-4b-5). S_ISDIR/S_ISREG read S_IFMT alone, so a bare
    // 0555 left a pid dir classified as no-type -- and every POSIX walker that
    // decides whether to descend (find, nftw, a shell glob, Go's IsDir) read it
    // as not-a-directory and stopped. qid_type already said QTDIR for native
    // callers; this is the same fact in the shape a ported one reads.
    TEST_EXPECT_EQ(devproc.stat_native(piddir, &st), 0, "stat_native(piddir) ok");
    TEST_EXPECT_EQ(st.mode & (u32)T_S_IFMT, (u32)T_S_IFDIR,
                   "pid dir S_IFMT = S_IFDIR (S_ISDIR is true)");
    TEST_EXPECT_EQ(st.mode & ~(u32)T_S_IFMT, (u32)0555u, "pid dir perms = 0555");
    TEST_EXPECT_EQ(st.qid_type, QTDIR,        "pid dir is QTDIR");
    spoor_unref(piddir);

    // The apex has no per-Proc owner, but it IS a directory and must STAT as
    // one (V-4b-4). It used to answer -1, which spoor_stat_native surfaces as
    // EIO -- so stat("/proc") failed, and realpath() on any path under /proc
    // with it (musl's resolver walks each prefix). SYSTEM-owned + 0555, the
    // devdev DEV_KIND_ROOT / devramfs synth-dir posture.
    struct Spoor *root2 = devproc.attach("");
    TEST_EXPECT_EQ(devproc.stat_native(root2, &st), 0,
                   "stat_native(dev apex) ok (it is a real directory)");
    TEST_EXPECT_EQ(st.qid_type, QTDIR,          "apex is QTDIR");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFDIR | 0555u),
                   "apex mode = S_IFDIR|0555 (world-searchable)");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "apex uid = SYSTEM");
    TEST_EXPECT_EQ(st.gid, (u32)GID_SYSTEM,       "apex gid = SYSTEM");
    spoor_unref(root2);

    // Leading zeros do NOT name a Proc (V-4b-5), Linux's own rule
    // (fs/proc/base.c::name_to_int). Without it one Proc answers to unboundedly
    // many names, so the pid -> name map stops being injective and native /proc
    // disagrees with the diorama (whose parse_pid already rejects them) about
    // whether a path exists. "0" itself stays legal -- kproc is pid 0.
    struct Spoor *root3 = devproc.attach("");
    TEST_ASSERT(root3 != NULL, "attach for the leading-zero probe");
    char padded[16];
    padded[0] = '0';
    for (int i = 0; i <= pn; i++) padded[1 + i] = pidstr[i];   // "0" + pid + NUL
    struct Spoor *bogus = walk_one(root3, padded);
    TEST_EXPECT_EQ(bogus, NULL, "walk to a zero-padded pid is a miss");
    if (bogus) spoor_unref(bogus);
    // ... and the un-padded spelling still resolves, so the reject is narrow.
    struct Spoor *plain = walk_one(root3, pidstr);
    TEST_ASSERT(plain != NULL, "the un-padded pid still walks");
    spoor_unref(plain);
    spoor_unref(root3);

    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);
}

// The kill dispatch: an authorized write terminates the target Proc's
// thread-group (group_exit_msg set); a denied / non-ALIVE write does not.
// Synthetic targets have no running thread, so the group_exit_msg flag is the
// observable (the death step is the audited #809/#811 EL0-die-check path).
void test_devproc_write_ctl_kill_dispatch(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "test thread has a proc");
    struct Proc *caller = t->proc;
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_KILL)),
                "test caller lacks CAP_HOSTOWNER/CAP_KILL (denied case is meaningful)");

    const char kill_cmd[]    = "kill";
    const char killgrp_cmd[] = "killgrp";
    const long kn  = (long)sizeof(kill_cmd) - 1;     // 4
    const long kgn = (long)sizeof(killgrp_cmd) - 1;  // 7

    // (a) OWNER-authorized kill: target owned by the caller's principal.
    struct Proc *owned = proc_alloc();
    TEST_ASSERT(owned != NULL, "alloc owned target");
    owned->principal_id = caller->principal_id;
    owned->state        = PROC_STATE_ALIVE;
    proc_test_link(owned);
    struct Spoor *octl = open_ctl_for_pid(owned->pid);
    TEST_ASSERT(octl != NULL, "open owned-target ctl");
    TEST_EXPECT_EQ(devproc.write(octl, kill_cmd, kn, 0), kn, "owner kill returns n");
    TEST_ASSERT(owned->group_exit_msg != NULL,
                "owned target group_exit_msg set (terminated)");
    spoor_clunk(octl);
    proc_test_unlink(owned);
    owned->state = PROC_STATE_ZOMBIE;
    proc_free(owned);

    // (b) DENIED: target owned by a different principal; caller holds no cap.
    struct Proc *other = proc_alloc();
    TEST_ASSERT(other != NULL, "alloc non-owned target");
    // A distinct, non-sentinel principal: guaranteed != caller by construction,
    // and never PRINCIPAL_INVALID(0) / SYSTEM(0xFFFFFFFE) / NONE(0xFFFFFFFF).
    // (A-4b audit F3: caller->principal_id + 1u could land on the PRINCIPAL_NONE
    // sentinel when the harness caller is PRINCIPAL_SYSTEM.)
    other->principal_id = (caller->principal_id == 0x0B0B0B0Bu) ? 0x0C0C0C0Cu
                                                                : 0x0B0B0B0Bu;
    other->state        = PROC_STATE_ALIVE;
    proc_test_link(other);
    struct Spoor *nctl = open_ctl_for_pid(other->pid);
    TEST_ASSERT(nctl != NULL, "open non-owned-target ctl");
    TEST_EXPECT_EQ(devproc.write(nctl, kill_cmd, kn, 0), (long)-1,
                   "non-owner with no cap is denied (-1)");
    TEST_EXPECT_EQ(other->group_exit_msg, (const char *)NULL,
                   "denied target NOT terminated (group_exit_msg NULL)");
    spoor_clunk(nctl);
    proc_test_unlink(other);
    other->state = PROC_STATE_ZOMBIE;
    proc_free(other);

    // (c) killgrp on an owned target also terminates (uniform dispatch).
    struct Proc *grp = proc_alloc();
    TEST_ASSERT(grp != NULL, "alloc killgrp target");
    grp->principal_id = caller->principal_id;
    grp->state        = PROC_STATE_ALIVE;
    proc_test_link(grp);
    struct Spoor *gctl = open_ctl_for_pid(grp->pid);
    TEST_ASSERT(gctl != NULL, "open killgrp-target ctl");
    TEST_EXPECT_EQ(devproc.write(gctl, killgrp_cmd, kgn, 0), kgn, "owner killgrp returns n");
    TEST_ASSERT(grp->group_exit_msg != NULL, "killgrp terminates the target");
    spoor_clunk(gctl);
    proc_test_unlink(grp);
    grp->state = PROC_STATE_ZOMBIE;
    proc_free(grp);

    // (d) a non-ALIVE target is refused even for the owner.
    struct Proc *dead = proc_alloc();
    TEST_ASSERT(dead != NULL, "alloc zombie target");
    dead->principal_id = caller->principal_id;
    dead->state        = PROC_STATE_ZOMBIE;
    proc_test_link(dead);
    struct Spoor *dctl = open_ctl_for_pid(dead->pid);
    TEST_ASSERT(dctl != NULL, "open zombie-target ctl");
    TEST_EXPECT_EQ(devproc.write(dctl, kill_cmd, kn, 0), (long)-1,
                   "kill of a non-ALIVE target is refused (-1)");
    TEST_EXPECT_EQ(dead->group_exit_msg, (const char *)NULL,
                   "non-ALIVE target not terminated");
    spoor_clunk(dctl);
    proc_test_unlink(dead);
    proc_free(dead);
}

// prowl-4: the job-control suspend/resume verb dispatch end-to-end (parse -> the
// I-26 gate -> the job_stop_req flip). Proves (a) the owner path flips
// job_stop_req WITHOUT terminating (strictly weaker than kill), idempotently;
// (b) the SAME I-26 gate as kill denies a non-owner-no-cap caller (the deny is
// non-vacuous -- the harness caller holds neither CAP_HOSTOWNER nor CAP_KILL);
// (c) a non-ALIVE target is refused even for the owner. The suspend is
// NON-TERMINAL, so unlike the kill test it asserts the target survives.
void test_devproc_ctl_suspend_resume_dispatch(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "test thread has a proc");
    struct Proc *caller = t->proc;
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_KILL)),
                "test caller lacks CAP_HOSTOWNER/CAP_KILL (denied case is meaningful)");

    const char suspend_cmd[] = "suspend";
    const char resume_cmd[]  = "resume";
    const long sn = (long)sizeof(suspend_cmd) - 1;   // 7
    const long rn = (long)sizeof(resume_cmd) - 1;    // 6

    // (a) OWNER-authorized suspend + resume: the job_stop_req flip, non-terminal.
    struct Proc *owned = proc_alloc();
    TEST_ASSERT(owned != NULL, "alloc owned target");
    owned->principal_id = caller->principal_id;
    owned->state        = PROC_STATE_ALIVE;
    proc_test_link(owned);
    struct Spoor *octl = open_ctl_for_pid(owned->pid);
    TEST_ASSERT(octl != NULL, "open owned-target ctl");

    TEST_EXPECT_EQ(devproc.write(octl, suspend_cmd, sn, 0), sn, "owner suspend returns n");
    TEST_EXPECT_EQ((int)owned->job_stop_req, 1, "suspend set job_stop_req");
    TEST_ASSERT(owned->stop_report_pending, "suspend latched the WAIT_UNTRACED report");
    TEST_EXPECT_EQ(owned->group_exit_msg, (const char *)NULL,
                   "suspend does NOT terminate -- strictly weaker than kill");

    // Idempotent: a second suspend does not re-latch the report (the primitive's
    // already-stopped guard). Consume the latch first to observe the no-re-arm.
    owned->stop_report_pending = false;
    TEST_EXPECT_EQ(devproc.write(octl, suspend_cmd, sn, 0), sn, "second suspend returns n");
    TEST_ASSERT(!owned->stop_report_pending, "idempotent suspend did NOT re-latch");

    TEST_EXPECT_EQ(devproc.write(octl, resume_cmd, rn, 0), rn, "owner resume returns n");
    TEST_EXPECT_EQ((int)owned->job_stop_req, 0, "resume cleared job_stop_req");
    TEST_ASSERT(owned->cont_report_pending, "resume latched the WAIT_CONTINUED report");

    spoor_clunk(octl);
    proc_test_unlink(owned);
    owned->state = PROC_STATE_ZOMBIE;
    proc_free(owned);

    // (b) DENIED: a non-owned target, caller holds no cap -- the SAME I-26 gate
    // as kill. The target must NOT be stopped.
    struct Proc *other = proc_alloc();
    TEST_ASSERT(other != NULL, "alloc non-owned target");
    other->principal_id = (caller->principal_id == 0x0B0B0B0Bu) ? 0x0C0C0C0Cu
                                                                : 0x0B0B0B0Bu;
    other->state        = PROC_STATE_ALIVE;
    proc_test_link(other);
    struct Spoor *nctl = open_ctl_for_pid(other->pid);
    TEST_ASSERT(nctl != NULL, "open non-owned-target ctl");
    TEST_EXPECT_EQ(devproc.write(nctl, suspend_cmd, sn, 0), (long)-1,
                   "non-owner suspend denied (-1) -- the I-26 gate");
    TEST_EXPECT_EQ((int)other->job_stop_req, 0, "denied target NOT stopped");
    spoor_clunk(nctl);
    proc_test_unlink(other);
    other->state = PROC_STATE_ZOMBIE;
    proc_free(other);

    // (c) a non-ALIVE target is refused even for the owner (mirrors kill (d)).
    struct Proc *dead2 = proc_alloc();
    TEST_ASSERT(dead2 != NULL, "alloc zombie target");
    dead2->principal_id = caller->principal_id;
    dead2->state        = PROC_STATE_ZOMBIE;
    proc_test_link(dead2);
    struct Spoor *dctl2 = open_ctl_for_pid(dead2->pid);
    TEST_ASSERT(dctl2 != NULL, "open zombie-target ctl");
    TEST_EXPECT_EQ(devproc.write(dctl2, suspend_cmd, sn, 0), (long)-1,
                   "suspend of a non-ALIVE target refused even for the owner");
    TEST_EXPECT_EQ((int)dead2->job_stop_req, 0, "non-ALIVE target NOT stopped");
    spoor_clunk(dctl2);
    proc_test_unlink(dead2);
    proc_free(dead2);
}

// 8a-1b: the I-39 debug-authority predicate (owner OR CAP_DEBUG; kproc +
// NOTRACE refused; CAP_HOSTOWNER/CAP_DAC_OVERRIDE are NOT debug axes at v1.0).
void test_devproc_debug_authorized_predicate(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");

    target->principal_id = 0xA11CEu;
    target->primary_gid  = 0x6u;
    target->state        = PROC_STATE_ALIVE;

    // 1. Different principal, no caps -> denied.
    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "non-owner with no caps cannot debug");

    // 2. Same principal (owner) -> allowed.
    caller->principal_id = 0xA11CEu;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "the owner (same principal) can debug");

    // 3. Different principal + CAP_DEBUG -> allowed (cross-identity debug).
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_DEBUG;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "CAP_DEBUG authorizes a cross-identity debug");

    // 4. Different principal + CAP_HOSTOWNER -> allowed (the host owner / Plan 9
    //    "eve" is a debug axis, user-voted 2026-07-15; the I-26 kill-gate analog).
    caller->caps = CAP_HOSTOWNER;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "CAP_HOSTOWNER authorizes a debug (host owner / eve)");

    // 5. Different principal + CAP_DAC_OVERRIDE -> DENIED (fs-admin != debug).
    caller->caps = CAP_DAC_OVERRIDE;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "CAP_DAC_OVERRIDE is NOT a debug axis");

    // 6. kproc (pid 0) is NEVER debuggable, even for a CAP_DEBUG holder (refused
    //    before the authority axes).
    caller->caps = CAP_DEBUG;
    TEST_ASSERT(!devproc_debug_authorized(caller, kproc()),
                "kproc is undebuggable");

    // 7. A PROC_FLAG_NOTRACE target is refused, even for the owner AND a
    //    CAP_DEBUG holder (the no-trace seam, DEBUG-FS section 8).
    proc_seal(target, PROC_FLAG_NOTRACE);
    caller->principal_id = target->principal_id;    // owner
    caller->caps         = 0;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "NOTRACE refuses the owner");
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_DEBUG;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "NOTRACE refuses a CAP_DEBUG holder");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

// The capability-cover rule (DEBUG-FS-DESIGN 3.1, scripture 389c06b9): the owner
// axis admits only while the caller's caps COVER the target's. RED before the
// fix -- a same-principal caller with NO caps was admitted to a target holding
// CAP_KILL, which is how an unelevated shell reached its own imperium-elevated
// sub-shell (and any propagating-scope member), borrowing a trusted-path
// elevation (I-25/I-27) it never went through the trusted path to get.
//
// Every leg restores nothing on the CALLER here: both Procs are synthetic, so
// their caps are set freely. The cap-axis legs deliberately use a caller whose
// caps do NOT cover, proving the cap axis still overrides the cover refusal.
void test_devproc_debug_cap_cover_predicate(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");

    target->principal_id = 0xA11CEu;
    target->state        = PROC_STATE_ALIVE;
    caller->principal_id = 0xA11CEu;              // the OWNER axis, every leg below

    // 1. Equal authority -> admitted (the common case: a peer of the same power).
    target->caps = 0;
    caller->caps = 0;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "cover: equal caps (both empty) admits");

    // 2. The caller's caps are a STRICT SUPERSET -> admitted. This is the
    //    shell-debugs-its-child case, and I-2 guarantees it: fork-grantable caps
    //    only shrink, so a spawner always covers its children.
    target->caps = CAP_KILL;
    caller->caps = CAP_KILL | CAP_CHOWN;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "cover: a superset caller admits (the spawner case, I-2)");

    // 3. Exact cover -> admitted.
    caller->caps = CAP_KILL;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "cover: an exactly-covering caller admits");

    // 4. THE REGRESSION: one cap the caller lacks -> REFUSED on the owner axis.
    caller->caps = 0;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "cover: a same-principal caller lacking the target's CAP_KILL is refused");

    // 5. A DISJOINT cap set is not cover either (the caller is powerful, but not
    //    in the way the target is) -- a subset test, never a "has any caps" test.
    caller->caps = CAP_CHOWN;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "cover: disjoint caps are not cover");

    // 6. The cap axis still overrides a cover refusal, both bits, with the
    //    caller's own set still NOT covering the target.
    caller->caps = CAP_DEBUG;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "cover: CAP_DEBUG admits an uncovered owner-axis target");
    caller->caps = CAP_HOSTOWNER;
    TEST_ASSERT(devproc_debug_authorized(caller, target),
                "cover: CAP_HOSTOWNER admits an uncovered owner-axis target");

    // 7. CAP_DAC_OVERRIDE is still not a debug axis, and being merely "elevated"
    //    is not cover -- the fs-admin bit cannot buy a debug it does not cover.
    caller->caps = CAP_DAC_OVERRIDE;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "cover: CAP_DAC_OVERRIDE neither covers nor is a debug axis");

    // 8. The NOTRACE seam still wins over a FULLY-COVERING owner (the seal keeps
    //    its job for equal-authority peers -- it is not made redundant).
    proc_seal(target, PROC_FLAG_NOTRACE);
    caller->caps = CAP_KILL;
    TEST_ASSERT(!devproc_debug_authorized(caller, target),
                "cover: NOTRACE still refuses a covering owner");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

// The same rule END TO END, through devproc's own `attach` verb rather than the
// predicate -- a SEPARATE test because TEST_ASSERT returns on the first failure,
// so a predicate regression must not be able to hide the real attach path's.
// The attach verb consults only ALIVE, this gate and Einuse, so the gate is the
// only thing that can refuse here.
void test_devproc_debug_cap_cover_attach(void) {
    struct Thread *th = current_thread();
    TEST_ASSERT(th && th->proc, "test thread has a proc");
    struct Proc *caller = th->proc;

    struct Proc *elev = proc_alloc();
    TEST_ASSERT(elev != NULL, "alloc the elevated target");
    elev->principal_id = caller->principal_id;     // same principal (I-22)
    elev->state        = PROC_STATE_ALIVE;
    elev->caps         = CAP_KILL;                 // a cap the bare caller lacks
    proc_test_link(elev);

    const char attach_cmd[] = "attach";
    const char detach_cmd[] = "detach";
    const long an = (long)sizeof(attach_cmd) - 1;
    const long dn = (long)sizeof(detach_cmd) - 1;

    // The caller is the LIVE runner Proc (kproc), so its caps are saved and
    // restored BEFORE any assertion -- TEST_ASSERT returns, and leaking a
    // narrowed cap set into the rest of the suite would be a fixture that
    // generates its own bugs.
    caps_t saved = __atomic_load_n(&caller->caps, __ATOMIC_ACQUIRE);
    // The premise the refusal leg rests on, asserted rather than assumed (before
    // any mutation, so this return is clean): CAP_KILL is elevation-only and so
    // excluded from CAP_ALL, which is the runner's set. The refusal leg therefore
    // needs NO caller mutation at all -- an untouched caller already fails cover
    // against this target, which is strictly better discrimination, because
    // nothing the fixture did can be blamed for the refusal.
    TEST_ASSERT((saved & CAP_KILL) == 0,
                "premise: the runner lacks the target's CAP_KILL (else the refusal is vacuous)");

    struct Spoor *ctl = open_ctl_for_pid(elev->pid);
    long bare_ret  = ctl ? devproc.write(ctl, attach_cmd, an, 0) : -2;
    void *bare_own = ctl ? (void *)elev->debug_owner : (void *)-1;
    if (ctl && bare_ret == an) (void)devproc.write(ctl, detach_cmd, dn, 0);

    // The control, one variable away: the SAME caller and target, caps now
    // covering. Without it, a broken fixture (an unopenable ctl, a dead target)
    // would satisfy the refusal above on its own. WIDENS the shared runner set by
    // the one elevation-only bit rather than replacing it, so the window cannot
    // strip CAP_HW_CREATE / CAP_TCB_DIAL / CAP_CSPRNG_READ from a kproc-context
    // path running beside it.
    __atomic_store_n(&caller->caps, saved | (caps_t)CAP_KILL, __ATOMIC_RELEASE);
    long cover_ret  = ctl ? devproc.write(ctl, attach_cmd, an, 0) : -2;
    void *cover_own = ctl ? (void *)elev->debug_owner : (void *)-1;
    if (ctl && cover_ret == an) (void)devproc.write(ctl, detach_cmd, dn, 0);

    __atomic_store_n(&caller->caps, saved, __ATOMIC_RELEASE);
    if (ctl) spoor_clunk(ctl);
    proc_test_unlink(elev);
    elev->state = PROC_STATE_ZOMBIE;
    proc_free(elev);

    TEST_ASSERT(ctl != NULL, "open the elevated target's ctl");
    TEST_EXPECT_EQ(cover_ret, an, "control: a covering owner's attach succeeds");
    TEST_EXPECT_EQ(cover_own, (void *)ctl, "control: the covering attach claimed the slot");
    TEST_EXPECT_EQ(bare_ret, (long)-1, "an uncovered same-principal attach is refused");
    TEST_EXPECT_EQ(bare_own, (void *)NULL, "the refused attach claimed no slot");
}

// The DUMP SEAL's predicates (DEBUG-FS-DESIGN 3.2, operator-delegated 2026-09-24).
// PROC_FLAG_NODUMP means "cannot be EXTRACTED FROM" and PROC_FLAG_NOTRACE "cannot be
// DRIVEN" -- Linux's split, where dumpability and not the ptrace flag governs
// /proc/<pid> content. RED before the fix: nothing read NODUMP, so a SEALED Proc's
// environment was readable by any same-principal peer -- the exact actor
// SPAWN_PERM_SEAL exists to shut out.
//
// Every verdict is CAPTURED, the Procs are freed, and only then is anything
// asserted: TEST_ASSERT returns, and a fixture released on the last line is released
// only by a passing test. Leg ORDER still matters -- both CONTROLS are taken while the
// target is unsealed, so no refusal below can be satisfied by a caller that passes on
// no axis -- and the seal is set ONCE, through proc_seal (the production writer),
// because it is one-way and a test that cleared it would assert against a state the
// kernel cannot reach.
void test_devproc_dump_seal_predicate(void) {
    struct Proc *caller       = proc_alloc();
    struct Proc *target       = proc_alloc();
    struct Proc *notrace_only = proc_alloc();
    bool allocated = caller && target && notrace_only;
    bool premise = false, ctl_owner = false, ctl_host = false, mirror_premise = false;
    bool mirror = false, sealed_host = true, sealed_peer = true, self = false;
    bool debug_premise = false, debug_axis = false;
    if (allocated) {
        target->principal_id = 0x5EA1Eu;
        target->state        = PROC_STATE_ALIVE;
        target->caps         = 0;
        caller->principal_id = 0x5EA1Eu;              // the OWNER axis
        caller->caps         = 0;
        premise = (target->proc_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE)) == 0;

        // 1. CONTROL, owner axis: an unsealed same-principal target discloses.
        ctl_owner = devproc_extract_authorized(caller, target);
        // 2. CONTROL, cap axis: CAP_HOSTOWNER discloses an unsealed cross-principal
        //    target. Taken while unsealed, for the reason in the header.
        caller->principal_id = 0xD1FFu;
        caller->caps         = CAP_HOSTOWNER;
        ctl_host = devproc_extract_authorized(caller, target);

        // 3. THE MIRROR LEG (round-1 P3-7), on its OWN Proc: NOTRACE alone must NOT
        //    refuse extraction. Without it a predicate written `& (NODUMP | NOTRACE)`
        //    passes every other leg while denying every Proc that called only
        //    SYS_SET_TRACEABLE(0) -- a symmetric check cannot catch a symmetric fault.
        //    Its own Proc because the bits are one-way: NOTRACE set on the shared
        //    target once silently invalidated leg 7's premise.
        notrace_only->principal_id = caller->principal_id;
        notrace_only->state        = PROC_STATE_ALIVE;
        notrace_only->caps         = 0;
        proc_seal(notrace_only, PROC_FLAG_NOTRACE);
        mirror_premise = (notrace_only->proc_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE))
                         == PROC_FLAG_NOTRACE;
        mirror = devproc_extract_authorized(caller, notrace_only);

        // 4. ABSOLUTE: CAP_HOSTOWNER does not buy through the seal. One variable
        //    away from leg 2.
        proc_seal(target, PROC_FLAG_NODUMP);
        sealed_host = devproc_extract_authorized(caller, target);
        // 5. THE REGRESSION: the same-principal PEER is refused. One variable away
        //    from leg 1.
        caller->principal_id = 0x5EA1Eu;
        caller->caps         = 0;
        sealed_peer = devproc_extract_authorized(caller, target);
        // 6. SELF is exempt: a Proc reads itself by its own pid (devproc has no
        //    `self` entry), and sealing it against itself protects nobody.
        self = devproc_extract_authorized(target, target);
        // 7. NODUMP is not a CONTROL refusal. The premise is captured, not assumed:
        //    this leg was once vacuous when an earlier leg left NOTRACE on `target`.
        debug_premise = (target->proc_flags & PROC_FLAG_NOTRACE) == 0;
        debug_axis    = devproc_debug_authorized(target, target);
    }
    if (caller)       { caller->state = PROC_STATE_ZOMBIE;       proc_free(caller); }
    if (target)       { target->state = PROC_STATE_ZOMBIE;       proc_free(target); }
    if (notrace_only) { notrace_only->state = PROC_STATE_ZOMBIE; proc_free(notrace_only); }

    TEST_ASSERT(allocated, "proc_alloc caller + target + the NOTRACE-only target");
    TEST_ASSERT(premise, "premise: the target starts with NEITHER seal bit");
    TEST_ASSERT(ctl_owner, "control: an unsealed same-principal target discloses");
    TEST_ASSERT(ctl_host, "control: CAP_HOSTOWNER discloses an unsealed cross-principal target");
    TEST_ASSERT(mirror_premise, "premise: the mirror target has NOTRACE and NOT NODUMP");
    TEST_ASSERT(mirror, "NOTRACE alone does NOT refuse extraction: it is the CONTROL bit");
    TEST_ASSERT(!sealed_host, "the dump seal refuses CAP_HOSTOWNER");
    TEST_ASSERT(!sealed_peer, "the dump seal refuses a same-principal peer's extraction");
    TEST_ASSERT(self, "a sealed Proc still extracts from ITSELF");
    TEST_ASSERT(debug_premise, "premise: the target carries NODUMP and NOT NOTRACE");
    TEST_ASSERT(debug_axis, "NODUMP does not refuse the NOTRACE-gated debug axis");
}

// The seal END TO END through every /proc/<pid> file the plain read path serves.
// This pins the SET at the call sites, which no predicate test can: round 2 showed
// that re-pointing environ back at the unsealed predicate, or imperium at the sealed
// one, left every predicate-level test green. Each file is read once unsealed (the
// control: an allowed read is >= 0, possibly 0 bytes on a synthetic Proc) and once
// after proc_seal (the one variable); an image file must then refuse (-1) and a
// ledger or control file must still answer. A second, CROSS-principal target pins
// environ's own owner gate the same way -- the in-kernel runner holds no
// CAP_HOSTOWNER (asserted below), so that deny leg is reachable end to end.
// mem / regs / fpregs are the debug tests' (they need a stopped target).
void test_devproc_dump_seal_disclosure(void) {
    static const struct {
        const char *name;
        bool        image;
        const char *control_msg;
        const char *sealed_msg;
    } files[] = {
        { "status",   false, "control: status reads while unsealed",
          "status is the kernel's record -- it still reads when sealed" },
        { "cmdline",  true,  "control: cmdline reads while unsealed",
          "the dump seal refuses cmdline" },
        { "ctl",      false, "control: ctl reads while unsealed",
          "ctl is a control file -- it still reads when sealed" },
        { "ns",       true,  "control: ns reads while unsealed",
          "the dump seal refuses ns (round-2 P1)" },
        { "exe",      true,  "control: exe reads while unsealed",
          "the dump seal refuses exe" },
        { "cwd",      true,  "control: cwd reads while unsealed",
          "the dump seal refuses cwd" },
        { "maps",     true,  "control: maps reads while unsealed",
          "the dump seal refuses maps" },
        { "sched",    false, "control: sched reads while unsealed",
          "sched is telemetry -- it still reads when sealed" },
        { "imperium", false, "control: imperium reads while unsealed",
          "imperium is the kernel's attestation -- it still reads when sealed (I-25)" },
        { "environ",  true,  "control: environ reads while unsealed",
          "the dump seal refuses environ" },
    };
    enum { NFILES = (int)(sizeof(files) / sizeof(files[0])) };

    struct Thread *th = current_thread();
    TEST_ASSERT(th && th->proc, "test thread has a proc");
    struct Proc *target = proc_alloc();
    struct Proc *other  = proc_alloc();
    bool allocated = target && other;
    struct Spoor *f[NFILES];
    long before[NFILES], after[NFILES];
    bool premise = false;
    long other_environ = -2, other_maps = -2;
    char buf[512];
    for (int i = 0; i < NFILES; i++) { f[i] = NULL; before[i] = -2; after[i] = -2; }

    if (allocated) {
        target->principal_id = th->proc->principal_id;   // the reader's own principal
        target->state        = PROC_STATE_ALIVE;
        target->caps         = 0;
        proc_test_link(target);
        premise = (target->proc_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE)) == 0;
        for (int i = 0; i < NFILES; i++) {
            f[i] = open_pidfile_for(target->pid, files[i].name, 0);   // OREAD
            before[i] = f[i] ? devproc.read(f[i], buf, (long)sizeof(buf), 0) : -2;
        }
        proc_seal(target, PROC_FLAG_NODUMP);
        for (int i = 0; i < NFILES; i++)
            after[i] = f[i] ? devproc.read(f[i], buf, (long)sizeof(buf), 0) : -2;
        for (int i = 0; i < NFILES; i++) if (f[i]) spoor_clunk(f[i]);
        proc_test_unlink(target);

        // environ's OWN gate, end to end: an UNSEALED target of another principal.
        // maps is its one-variable control -- ambient, so it must still read.
        other->principal_id = (th->proc->principal_id == 0x0D0D0D0Du) ? 0x0E0E0E0Eu
                                                                       : 0x0D0D0D0Du;
        other->state        = PROC_STATE_ALIVE;
        other->caps         = 0;
        proc_test_link(other);
        struct Spoor *oe = open_pidfile_for(other->pid, "environ", 0);
        struct Spoor *om = open_pidfile_for(other->pid, "maps", 0);
        other_environ = oe ? devproc.read(oe, buf, (long)sizeof(buf), 0) : -2;
        other_maps    = om ? devproc.read(om, buf, (long)sizeof(buf), 0) : -2;
        if (oe) spoor_clunk(oe);
        if (om) spoor_clunk(om);
        proc_test_unlink(other);
    }
    if (target) { target->state = PROC_STATE_ZOMBIE; proc_free(target); }
    if (other)  { other->state  = PROC_STATE_ZOMBIE; proc_free(other); }

    TEST_ASSERT(allocated, "alloc the sealed target + the cross-principal target");
    TEST_ASSERT(premise, "premise: the target starts with neither seal bit");
    TEST_ASSERT(!(__atomic_load_n(&th->proc->caps, __ATOMIC_ACQUIRE) & CAP_HOSTOWNER),
                "premise: the runner holds no CAP_HOSTOWNER, so the cross-principal leg means something");
    for (int i = 0; i < NFILES; i++) {
        TEST_ASSERT(before[i] >= 0, files[i].control_msg);
        if (files[i].image) TEST_ASSERT(after[i] == -1, files[i].sealed_msg);
        else                TEST_ASSERT(after[i] >= 0, files[i].sealed_msg);
    }
    TEST_ASSERT(other_maps >= 0, "control: an unsealed cross-principal maps read is ambient");
    TEST_EXPECT_EQ(other_environ, (long)-1,
                   "environ refuses a cross-principal reader that holds no CAP_HOSTOWNER");
}

// The seal's SCOPE at the predicate level: the extraction gate is sealed and the
// attestation gate is not. The same split is pinned at the CALL SITES by
// test_devproc_dump_seal_disclosure; this test exists so a predicate regression is
// reported as one. `imperium` and `sched` are the kernel's record ABOUT a Proc;
// sealing them would let any Proc in a live propagating legate scope permanently
// suppress the kernel's record of its own elevation, because SYS_SET_DUMPABLE(0) is
// an ungated one-way self-call. The first cut of the seal did exactly that.
void test_devproc_dump_seal_scope(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    struct Proc *host   = proc_alloc();
    bool allocated = caller && target && host;
    bool premise = false, agree = false, attest = false, extract = true;
    bool host_attest = false, host_extract = true;
    bool host_imp_denied = true, host_sched_denied = true;
    size_t host_imp = 0, host_sched = 0;
    if (allocated) {
        target->principal_id = 0x5EA1Eu;
        target->state        = PROC_STATE_ALIVE;
        target->caps         = 0;
        caller->principal_id = 0x5EA1Eu;
        caller->caps         = 0;
        premise = (target->proc_flags & PROC_FLAG_NODUMP) == 0;
        // Both gates agree while unsealed -- so the divergence below is the seal's
        // doing and not a difference the two predicates always had.
        agree = devproc_owner_or_hostowner(caller, target) &&
                devproc_extract_authorized(caller, target);
        proc_seal(target, PROC_FLAG_NODUMP);
        attest  = devproc_owner_or_hostowner(caller, target);
        extract = devproc_extract_authorized(caller, target);
        // CAP_HOSTOWNER of ANOTHER principal: the seal is absolute against it for
        // extraction, and the ledger still answers it -- the operator's view of an
        // audited Proc (I-25) must not hang on being its owner. Through the gated
        // readers the dispatch calls, not only their predicate.
        host->principal_id = (target->principal_id == 0x0B0B0B0Bu) ? 0x0C0C0C0Cu
                                                                   : 0x0B0B0B0Bu;
        host->caps         = CAP_HOSTOWNER;
        host_attest  = devproc_owner_or_hostowner(host, target);
        host_extract = devproc_extract_authorized(host, target);
        char hbuf[512];
        host_imp   = devproc_imperium_read_gated(host, target, hbuf, sizeof(hbuf),
                                                 &host_imp_denied);
        host_sched = devproc_sched_read_gated(host, target, hbuf, sizeof(hbuf),
                                              &host_sched_denied);
    }
    if (caller) { caller->state = PROC_STATE_ZOMBIE; proc_free(caller); }
    if (target) { target->state = PROC_STATE_ZOMBIE; proc_free(target); }
    if (host)   { host->state   = PROC_STATE_ZOMBIE; proc_free(host); }

    TEST_ASSERT(allocated, "proc_alloc caller + target");
    TEST_ASSERT(premise, "premise: the target starts unsealed");
    TEST_ASSERT(agree, "control: unsealed, the authority gate and the extraction gate agree");
    TEST_ASSERT(attest, "the ATTESTATION gate (imperium/sched) is NOT sealed -- an audited "
                        "Proc cannot switch off the kernel's record of its own elevation");
    TEST_ASSERT(!extract, "the EXTRACTION gate (environ) IS sealed");
    TEST_ASSERT(host_attest, "a cross-principal CAP_HOSTOWNER passes the attestation "
                             "gate on a sealed Proc");
    TEST_ASSERT(!host_extract, "the seal is absolute for extraction -- CAP_HOSTOWNER included");
    TEST_ASSERT(!host_imp_denied && host_imp > 0,
                "imperium still formats for a cross-principal CAP_HOSTOWNER when sealed (I-25)");
    TEST_ASSERT(!host_sched_denied && host_sched > 0,
                "sched still formats for a cross-principal CAP_HOSTOWNER when sealed");
}

// 8a-1b: the attach/detach/close slot lifecycle (the model's Attach / DetachReq
// / DbgDie -> ReleaseSlot). Proves: attach claims (Einuse on a 2nd attach),
// detach frees, and the ctl-fd CLOSE frees the slot with no explicit detach
// (the handle-lifetime-tied stop ownership -- the NoStrand foundation).
void test_devproc_debug_attach_detach_lifecycle(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "test thread has a proc");
    struct Proc *caller = t->proc;
    TEST_ASSERT(!(caller->caps & CAP_DEBUG),
                "test caller lacks CAP_DEBUG (the denied case is meaningful)");

    const char attach_cmd[] = "attach";
    const char detach_cmd[] = "detach";
    const long an = (long)sizeof(attach_cmd) - 1;   // 6
    const long dn = (long)sizeof(detach_cmd) - 1;   // 6

    // (a) OWNER attach claims the slot; a 2nd attach is Einuse; detach frees.
    struct Proc *owned = proc_alloc();
    TEST_ASSERT(owned != NULL, "alloc owned target");
    owned->principal_id = caller->principal_id;
    owned->state        = PROC_STATE_ALIVE;
    proc_test_link(owned);

    struct Spoor *ctl1 = open_ctl_for_pid(owned->pid);
    TEST_ASSERT(ctl1 != NULL, "open owned-target ctl #1");
    TEST_EXPECT_EQ(devproc.write(ctl1, attach_cmd, an, 0), an, "owner attach returns n");
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)ctl1,
                   "attach claims the slot (debug_owner == ctl #1)");
    TEST_ASSERT((ctl1->flag & CDEBUGOWNER) != 0, "attach marks the ctl Spoor CDEBUGOWNER");

    struct Spoor *ctl2 = open_ctl_for_pid(owned->pid);
    TEST_ASSERT(ctl2 != NULL, "open owned-target ctl #2");
    TEST_EXPECT_EQ(devproc.write(ctl2, attach_cmd, an, 0), (long)-1, "2nd attach is Einuse (-1)");
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)ctl1, "slot still owned by ctl #1");
    spoor_clunk(ctl2);   // ctl2 never owned the slot -> no release
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)ctl1,
                   "clunking a non-owner ctl leaves the slot");

    TEST_EXPECT_EQ(devproc.write(ctl1, detach_cmd, dn, 0), dn, "owner detach returns n");
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)NULL, "detach frees the slot");
    spoor_clunk(ctl1);

    // (b) the handle-lifetime-tied release: attach, then CLOSE the ctl fd
    //     (debugger death / fd close) frees the slot with no explicit detach.
    struct Spoor *ctl3 = open_ctl_for_pid(owned->pid);
    TEST_ASSERT(ctl3 != NULL, "re-open owned-target ctl");
    TEST_EXPECT_EQ(devproc.write(ctl3, attach_cmd, an, 0), an, "re-attach returns n");
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)ctl3, "re-attach claims the slot");
    spoor_clunk(ctl3);   // close the fd -> devproc_close releases the slot
    TEST_EXPECT_EQ((void *)owned->debug_owner, (void *)NULL,
                   "ctl-fd close releases the slot (handle-lifetime-tied, NoStrand)");

    proc_test_unlink(owned);
    owned->state = PROC_STATE_ZOMBIE;
    proc_free(owned);

    // (c) DENIED: a target owned by a different principal, caller no CAP_DEBUG.
    struct Proc *other = proc_alloc();
    TEST_ASSERT(other != NULL, "alloc non-owned target");
    other->principal_id = (caller->principal_id == 0x0B0B0B0Bu) ? 0x0C0C0C0Cu
                                                                : 0x0B0B0B0Bu;
    other->state        = PROC_STATE_ALIVE;
    proc_test_link(other);
    struct Spoor *nctl = open_ctl_for_pid(other->pid);
    TEST_ASSERT(nctl != NULL, "open non-owned-target ctl");
    TEST_EXPECT_EQ(devproc.write(nctl, attach_cmd, an, 0), (long)-1,
                   "non-owner without CAP_DEBUG is denied (-1)");
    TEST_EXPECT_EQ((void *)other->debug_owner, (void *)NULL, "denied target NOT attached");
    spoor_clunk(nctl);
    proc_test_unlink(other);
    other->state = PROC_STATE_ZOMBIE;
    proc_free(other);

    // (d) kproc (pid 0) attach is refused end-to-end (undebuggable kernel).
    struct Spoor *kctl = open_ctl_for_pid(0);
    TEST_ASSERT(kctl != NULL, "open /proc/0/ctl (kproc)");
    TEST_EXPECT_EQ(devproc.write(kctl, attach_cmd, an, 0), (long)-1,
                   "attach to kproc is refused (-1)");
    TEST_EXPECT_EQ((void *)kproc()->debug_owner, (void *)NULL, "kproc slot untouched");
    spoor_clunk(kctl);
}

// 5d EXITKILL (I-39 die-with-launcher; DEBUG-FS §5d; specs/debug_stop.tla
// EventuallyLaunchedDies / the exitkill_ignored cfg): the ctl-fd-close release
// TERMINATES a debugger-LAUNCHED (exitkill-marked) target instead of resuming it,
// so a launched debuggee dies with its debugger (the Plan 9 NoStrand-resume would
// orphan it to init to run forever -- the HVF-idle leak). Two legs on synthetic
// (thread-less) targets: (a) a marked target is TERMINATED on close
// (group_exit_msg set by proc_group_terminate); (b) an UNMARKED (attached, not
// launched) target is RESUMED on close (group_exit_msg stays NULL). Revert-probe:
// drop the release-cb exitkill branch (always proc_debug_resume) and leg (a) fails.
void test_devproc_debug_exitkill_terminates_on_close(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "test thread has a proc (the debugger/caller)");
    struct Proc *caller = t->proc;

    const char attach_cmd[]   = "attach";
    const char exitkill_cmd[] = "exitkill";
    const long an = (long)sizeof(attach_cmd) - 1;    // 6
    const long xn = (long)sizeof(exitkill_cmd) - 1;  // 8

    // (a) a LAUNCHED (exitkill-marked) target is TERMINATED when the debugger's
    //     ctl fd closes (debugger death) -- die-with-launcher.
    struct Proc *launched = proc_alloc();
    TEST_ASSERT(launched != NULL, "alloc launched target");
    launched->principal_id = caller->principal_id;   // owner axis -> attach passes
    launched->state        = PROC_STATE_ALIVE;
    proc_test_link(launched);

    struct Spoor *ctl = open_ctl_for_pid(launched->pid);
    TEST_ASSERT(ctl != NULL, "open launched-target ctl");
    TEST_EXPECT_EQ(devproc.write(ctl, attach_cmd, an, 0), an, "attach returns n");
    TEST_EXPECT_EQ((void *)launched->debug_owner, (void *)ctl, "attach claims the slot");
    TEST_ASSERT(launched->debug_exitkill == false, "a fresh attach leaves exitkill unset");

    TEST_EXPECT_EQ(devproc.write(ctl, exitkill_cmd, xn, 0), xn, "exitkill verb returns n");
    TEST_ASSERT(launched->debug_exitkill == true, "exitkill marks the target");
    TEST_EXPECT_EQ(launched->group_exit_msg, (const char *)NULL,
                   "the exitkill mark ALONE does not terminate (only the death-release does)");

    spoor_clunk(ctl);   // close the fd -> devproc_close -> release-cb: exitkill+ALIVE -> proc_group_terminate
    TEST_EXPECT_EQ((void *)launched->debug_owner, (void *)NULL, "close freed the slot");
    TEST_ASSERT(launched->group_exit_msg != NULL,
                "EXITKILL: a marked target is TERMINATED on debugger death (die-with-launcher)");
    TEST_ASSERT(launched->debug_exitkill == false, "the mark is cleared with the slot");

    proc_test_unlink(launched);
    launched->state = PROC_STATE_ZOMBIE;
    proc_free(launched);

    // (b) CONTROL: an ATTACHED (unmarked) target is RESUMED on close, NOT
    //     terminated -- the NoStrand-resume the EXITKILL refinement preserves.
    struct Proc *attached = proc_alloc();
    TEST_ASSERT(attached != NULL, "alloc attached target");
    attached->principal_id = caller->principal_id;
    attached->state        = PROC_STATE_ALIVE;
    proc_test_link(attached);

    struct Spoor *ctl2 = open_ctl_for_pid(attached->pid);
    TEST_ASSERT(ctl2 != NULL, "open attached-target ctl");
    TEST_EXPECT_EQ(devproc.write(ctl2, attach_cmd, an, 0), an, "attach returns n");
    TEST_ASSERT(attached->debug_exitkill == false, "unmarked (no exitkill verb sent)");

    spoor_clunk(ctl2);  // close -> release-cb: NOT exitkill -> proc_debug_resume (NoStrand)
    TEST_EXPECT_EQ((void *)attached->debug_owner, (void *)NULL, "close freed the slot");
    TEST_EXPECT_EQ(attached->group_exit_msg, (const char *)NULL,
                   "an UNMARKED target is RESUMED on close, NOT terminated (NoStrand preserved)");

    proc_test_unlink(attached);
    attached->state = PROC_STATE_ZOMBIE;
    proc_free(attached);
}

// 8a-1b-beta: the run-control state machine (specs/debug_stop.tla, the model's
// RequestStop / StartResume / Confirm / ReleaseSlot). Drives it end-to-end via
// ctl writes on a SYNTHETIC (thread-less) target: with no threads to park, the
// stop-wait scan is VACUOUSLY "fully stopped" (no non-EXITING thread to wait
// for), so `stop` sets the flag + returns without blocking -- exercising the
// deliver + the slot-owner gate + the scan + the release-resume without needing
// a real EL0 thread parked at the tail (that park/resume is the SMP gate + the
// in-guest probe's job at 8a-1c). Proves the DeathWinsOverStop tail order + the
// register-then-observe park are the MODEL's job (debug_stop.tla, TLC-green);
// this pins the ctl surface + the flag mechanism + the NoStrand release.
void test_devproc_debug_stop_start_resume(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "test thread has a proc");
    struct Proc *caller = t->proc;

    const char attach_cmd[]   = "attach";
    const char stop_cmd[]     = "stop";
    const char start_cmd[]    = "start";
    const char waitstop_cmd[] = "waitstop";
    const char detach_cmd[]   = "detach";
    const long an  = (long)sizeof(attach_cmd) - 1;    // 6
    const long sn  = (long)sizeof(stop_cmd) - 1;      // 4
    const long stn = (long)sizeof(start_cmd) - 1;     // 5
    const long wn  = (long)sizeof(waitstop_cmd) - 1;  // 8
    const long dn  = (long)sizeof(detach_cmd) - 1;    // 6

    // (0) The bare deliver/resume flag mechanism (no ctl): proc_debug_stop_deliver
    //     sets the flag; proc_debug_resume clears it; resume is idempotent.
    struct Proc *flagt = proc_alloc();
    TEST_ASSERT(flagt != NULL, "alloc flag target");
    flagt->state = PROC_STATE_ALIVE;
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 0, "fresh Proc: no stop pending (KP_ZERO)");
    proc_debug_stop_deliver(flagt);
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 1, "deliver sets debug_stop_req");
    proc_debug_resume(flagt);
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 0, "resume clears debug_stop_req");
    proc_debug_resume(flagt);
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 0, "resume is idempotent (stays 0)");

    // (0') SA-1 -- proc_debug_fault_stop (the EC-path / hardware-fire deliver)
    //      gates the stop on debug_owner under g_proc_table_lock. A fire that
    //      raced a detach (owner already cleared) MUST be a no-op: it may not set
    //      debug_stop_req (else the target parks with no debugger left to resume it
    //      -> the strand; specs/debug_stop.tla StopImpliesOwned). With a live owner
    //      it delivers exactly like the ctl `stop` verb. `debug_owner` is only
    //      compared to NULL here, so any non-NULL identity token stands in for a
    //      live ctl fd (never dereferenced). Non-vacuous: an ungated fault-stop
    //      returns 1 + sets the flag on the no-owner leg.
    struct Spoor *owner_sentinel = (struct Spoor *)flagt;   // non-NULL; never deref'd
    flagt->debug_owner = NULL;                              // detached: a fire in the race window
    TEST_EXPECT_EQ((int)proc_debug_fault_stop(flagt), 0,
                   "fault-stop with no owner does not deliver (SA-1)");
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 0,
                   "fault-stop with no owner set no flag (SA-1: no strand)");
    flagt->debug_owner = owner_sentinel;                   // attached: a live debugger owns the slot
    TEST_EXPECT_EQ((int)proc_debug_fault_stop(flagt), 1,
                   "fault-stop with a live owner delivers");
    TEST_EXPECT_EQ((int)flagt->debug_stop_req, 1,
                   "fault-stop with a live owner set debug_stop_req");
    flagt->debug_owner = NULL;                             // restore before teardown
    proc_debug_resume(flagt);                              // clear the flag we just set

    flagt->state = PROC_STATE_ZOMBIE;
    proc_free(flagt);

    // A thread-less, caller-owned target the debugger attaches to.
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc run-control target");
    tgt->principal_id = caller->principal_id;   // owner -> attach authorized
    tgt->state        = PROC_STATE_ALIVE;
    proc_test_link(tgt);

    // (a) stop/start on a NON-attached target are refused (the slot-owner gate is
    //     stricter than the attach gate -- you must attach first).
    struct Spoor *pre = open_ctl_for_pid(tgt->pid);
    TEST_ASSERT(pre != NULL, "open target ctl (pre-attach)");
    TEST_EXPECT_EQ(devproc.write(pre, stop_cmd, sn, 0), (long)-1,
                   "stop without attach is refused (not the slot owner)");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 0, "refused stop set no flag");
    TEST_EXPECT_EQ(devproc.write(pre, start_cmd, stn, 0), (long)-1,
                   "start without attach is refused");
    spoor_clunk(pre);

    // (b) attach, then the stop -> start cycle (thread-less -> stop returns n).
    struct Spoor *ctl = open_ctl_for_pid(tgt->pid);
    TEST_ASSERT(ctl != NULL, "open target ctl");
    TEST_EXPECT_EQ(devproc.write(ctl, attach_cmd, an, 0), an, "attach returns n");
    TEST_EXPECT_EQ((void *)tgt->debug_owner, (void *)ctl, "attach claims the slot");

    TEST_EXPECT_EQ(devproc.write(ctl, stop_cmd, sn, 0), sn,
                   "owner stop returns n (thread-less target is vacuously stopped)");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 1, "stop set debug_stop_req");

    // waitstop on an already-stopped target returns n immediately.
    TEST_EXPECT_EQ(devproc.write(ctl, waitstop_cmd, wn, 0), wn,
                   "waitstop on a stopped target returns n");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 1, "waitstop does not change the flag");

    TEST_EXPECT_EQ(devproc.write(ctl, start_cmd, stn, 0), stn, "start returns n");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 0, "start cleared debug_stop_req (StartResume)");

    // (c) a NON-owner ctl cannot stop/start (a 2nd fd that never attached).
    struct Spoor *stranger = open_ctl_for_pid(tgt->pid);
    TEST_ASSERT(stranger != NULL, "open a 2nd (non-owner) ctl");
    TEST_EXPECT_EQ(devproc.write(ctl, stop_cmd, sn, 0), sn, "re-stop by the owner");
    TEST_EXPECT_EQ(devproc.write(stranger, start_cmd, stn, 0), (long)-1,
                   "a non-owner start is refused (slot-owner gate)");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 1, "the non-owner start did NOT resume");
    spoor_clunk(stranger);

    // (d) detach while STOPPED resumes (ReleaseSlot -> NoStrand): the flag clears
    //     AND the slot frees in one step.
    TEST_EXPECT_EQ(devproc.write(ctl, detach_cmd, dn, 0), dn, "detach returns n");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 0,
                   "detach-while-stopped resumes the target (debug_stop_req cleared)");
    TEST_EXPECT_EQ((void *)tgt->debug_owner, (void *)NULL, "detach freed the slot");

    // (e) the ctl-fd CLOSE path also resumes: re-attach + stop, then close the fd
    //     (no explicit detach) -- the handle-lifetime-tied release resumes.
    TEST_EXPECT_EQ(devproc.write(ctl, attach_cmd, an, 0), an, "re-attach");
    TEST_EXPECT_EQ(devproc.write(ctl, stop_cmd, sn, 0), sn, "stop again");
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 1, "stopped again");
    spoor_clunk(ctl);   // close -> devproc_close -> release + resume
    TEST_EXPECT_EQ((int)tgt->debug_stop_req, 0,
                   "ctl-fd close resumes the target (ReleaseSlot via the close hook)");
    TEST_EXPECT_EQ((void *)tgt->debug_owner, (void *)NULL, "close freed the slot");

    // (F3, #95-audit) devproc_focus_thread selection: a matched in-list focus is
    // returned; a foreign / NULL focus falls back to head. The deterministic twin
    // of the /ambush-probe stage-C multi-M E2E (the kproc harness cannot reproduce a
    // real off-head fault-stop). Revert-probe: a loop that returns head regardless
    // of match fails the in-list-peer leg. devproc_focus_thread reads only pointer
    // identity + ->next_in_proc, so two zero-init stack Threads suffice (unlinked
    // before free so proc_free never touches them). Only ->next_in_proc + pointer
    // identity are read, so set those explicitly (NOT `= {0}`: a large struct
    // Thread zero-init would emit a memset the freestanding kernel does not link).
    extern struct Thread *devproc_focus_thread(struct Proc *target);
    struct Proc *ft = proc_alloc();
    TEST_ASSERT(ft != NULL, "alloc focus-select target");
    ft->state = PROC_STATE_ALIVE;
    struct Thread fhth, fpth;
    fhth.proc = ft;  fpth.proc = ft;
    fhth.next_in_proc = &fpth;  fpth.next_in_proc = NULL;
    struct Thread *ft_saved = ft->threads;   // NULL for a fresh proc_alloc
    ft->threads = &fhth;                       // head = fhth, in-list peer = fpth
    ft->debug_focus_thread = NULL;
    TEST_ASSERT(devproc_focus_thread(ft) == &fhth, "focus NULL -> head");
    ft->debug_focus_thread = &fpth;
    TEST_ASSERT(devproc_focus_thread(ft) == &fpth, "in-list peer focus -> that thread (not head)");
    ft->debug_focus_thread = &fhth;
    TEST_ASSERT(devproc_focus_thread(ft) == &fhth, "in-list head focus -> head");
    ft->debug_focus_thread = t;   // a foreign thread (t->proc == kproc/test-proc != ft)
    TEST_ASSERT(devproc_focus_thread(ft) == &fhth, "foreign focus -> head fallback (the validation is load-bearing)");
    ft->threads = ft_saved;        // unlink the stack Threads before free
    ft->debug_focus_thread = NULL;
    ft->state = PROC_STATE_ZOMBIE;
    proc_free(ft);

    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);
}

// 8a-2c F1: a whole-Proc stop SUPERSEDES an in-flight single-step. proc_debug_
// stop_deliver clears every thread's debug_ss_armed + debug_stepover_va, so a step
// that a peer's bp/wp fire (or a detach/re-attach) interrupted before its own EC
// 0x32 does NOT leak a spurious SPSR.SS into the next resume (a phantom
// one-instruction stop after a `continue`). A minimal synthetic head thread
// carries the pending step; no kstack/trapframe needed (F1 touches only the two
// step flags). Non-vacuous: pre-fix the deliver left debug_ss_armed set.
void test_devproc_debug_step_cancel_on_stop(void) {
    struct Thread *tt = current_thread();
    TEST_ASSERT(tt && tt->proc, "test thread has a proc");

    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc step-cancel target");
    tgt->state = PROC_STATE_ALIVE;

    struct Thread th;
    for (size_t i = 0; i < sizeof(th); i++) ((u8 *)&th)[i] = 0;
    th.magic             = THREAD_MAGIC;
    th.state             = THREAD_SLEEPING;
    th.next_in_proc      = NULL;
    th.debug_ss_armed    = true;          // a single-step is in flight...
    th.debug_stepover_va = 0xBEEF000ull;  // ...over this bp
    tgt->threads         = &th;

    // A whole-Proc stop (the raw deliver -- the cancel walk lives in it) must
    // cancel the pending step. proc_debug_stop_deliver's g_proc_table_lock contract
    // is satisfied vacuously here (a fresh synthetic Proc, single-threaded harness,
    // no concurrent thread-list mutation -- the same direct call the section-0 leg
    // of debug_stop_start_resume uses). Capture BEFORE cleanup/assert (the kregs
    // lesson: a returning TEST_ASSERT would strand tgt with a dangling stack thread).
    proc_debug_stop_deliver(tgt);
    int ss_cleared = (th.debug_ss_armed == false);
    int va_cleared = (th.debug_stepover_va == 0);
    int req_set    = (int)tgt->debug_stop_req;

    tgt->threads = NULL;              // un-dangle before proc_free
    tgt->state   = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    TEST_EXPECT_EQ(ss_cleared, 1, "F1: a whole-Proc stop clears debug_ss_armed (step superseded)");
    TEST_EXPECT_EQ(va_cleared, 1, "F1: a whole-Proc stop clears debug_stepover_va");
    TEST_EXPECT_EQ(req_set,    1, "the deliver still set debug_stop_req");
}

// 8a-1b-gamma: /proc/<pid>/mem -- cross-Proc user memory RW (I-39; DEBUG-FS 4.5).
// Two layers: (1) the raw mmu_cross_proc_read/write resolver against a real Proc
// pgtable (RW read+write land; an RO leaf write is REFUSED [I-12 W^X / I-36 Image
// cache]; a non-resident VA -> 0, no fault-in); (2) the devproc mem-file path
// (the I-39 owner gate + the stopped-only gate + the copy) on a SYNTHETIC
// thread-less target -- debug_stop_req=1 with no threads is vacuously fully
// stopped, so the full walk_cb runs without a real EL0 thread parked at the tail
// (the in-guest E2E on a genuinely-parked target is 8a-1c).
void test_devproc_debug_mem(void) {
    struct Thread *tt = current_thread();
    TEST_ASSERT(tt && tt->proc, "test thread has a proc");
    struct Proc *caller = tt->proc;

    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc mem target (with a real pgtable_root)");
    TEST_ASSERT(tgt->as && tgt->as->pgtable_root != 0, "target has a pgtable_root");
    tgt->principal_id = caller->principal_id;   // owner -> I-39 authorized
    tgt->state        = PROC_STATE_ALIVE;

    const u64 RW_VA  = 0x20000000ull;   // 512 MiB, user-half
    const u64 RO_VA  = 0x20001000ull;   // + one page
    const u64 GAP_VA = 0x20002000ull;   // + two pages: never mapped (a hole)

    struct page *rw_pg = alloc_pages(0, KP_ZERO);
    struct page *ro_pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(rw_pg && ro_pg, "alloc backing pages");
    paddr_t rw_pa = page_to_pa(rw_pg), ro_pa = page_to_pa(ro_pg);
    u8 *rw_kva = (u8 *)pa_to_kva(rw_pa);
    u8 *ro_kva = (u8 *)pa_to_kva(ro_pa);
    for (int i = 0; i < 64; i++) { rw_kva[i] = (u8)(0xA0 + i); ro_kva[i] = (u8)(0x50 + i); }
    TEST_EXPECT_EQ(mmu_install_user_pte(tgt->as, proc_resource_exempt(tgt), RW_VA, rw_pa, VMA_PROT_RW,   false), 0, "map RW page");
    TEST_EXPECT_EQ(mmu_install_user_pte(tgt->as, proc_resource_exempt(tgt), RO_VA, ro_pa, VMA_PROT_READ, false), 0, "map RO page");

    // --- Layer 1: the raw cross-Proc resolver ---
    u8 buf[64], wbuf[64];
    long got = mmu_cross_proc_read(tgt->as->pgtable_root, RW_VA, buf, 64);
    TEST_EXPECT_EQ(got, 64L, "cross_proc_read reads the RW page span");
    bool match = true; for (int i = 0; i < 64; i++) if (buf[i] != (u8)(0xA0 + i)) match = false;
    TEST_ASSERT(match, "cross_proc_read returns the RW page bytes");

    for (int i = 0; i < 64; i++) wbuf[i] = (u8)(0x11 + i);
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->as->pgtable_root, RW_VA, wbuf, 64), 64L, "cross_proc_write writes the RW page");
    match = true; for (int i = 0; i < 64; i++) if (rw_kva[i] != (u8)(0x11 + i)) match = false;
    TEST_ASSERT(match, "cross_proc_write landed the bytes in the target page");

    // RO leaf: read OK, write REFUSED (W^X / Image cache) + the page untouched.
    TEST_EXPECT_EQ(mmu_cross_proc_read(tgt->as->pgtable_root, RO_VA, buf, 64), 64L, "cross_proc_read reads an RO page");
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->as->pgtable_root, RO_VA, wbuf, 64), 0L, "cross_proc_write REFUSES an RO leaf");
    TEST_ASSERT(ro_kva[0] == 0x50, "the RO page was NOT modified by the refused write");

    // A non-resident VA -> 0 (not resident; no fault-in).
    TEST_EXPECT_EQ(mmu_cross_proc_read(tgt->as->pgtable_root,  GAP_VA, buf,  64), 0L, "read of a hole returns 0");
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->as->pgtable_root, GAP_VA, wbuf, 64), 0L, "write of a hole returns 0");

    // --- Layer 2: the devproc mem-file path (I-39 + stopped-only) ---
    proc_test_link(tgt);
    struct Spoor *mem = open_mem_for_pid(tgt->pid);
    TEST_ASSERT(mem != NULL, "open /proc/<pid>/mem");

    // NOT stopped -> refused (stopped-only; DEBUG-FS 3).
    tgt->debug_stop_req = 0;
    TEST_EXPECT_EQ(devproc.read(mem, buf, 64, (s64)RW_VA), (long)-1, "mem read of a NOT-stopped target is refused");
    TEST_EXPECT_EQ(devproc.write(mem, wbuf, 64, (s64)RW_VA), (long)-1, "mem write of a NOT-stopped target is refused");

    // Stopped (thread-less -> vacuously fully-stopped) -> read/write work.
    tgt->debug_stop_req = 1;
    got = devproc.read(mem, buf, 64, (s64)RW_VA);
    TEST_EXPECT_EQ(got, 64L, "mem read of a stopped target returns the bytes");
    match = true; for (int i = 0; i < 64; i++) if (buf[i] != (u8)(0x11 + i)) match = false;   // the layer-1 write
    TEST_ASSERT(match, "mem read returns the RW page bytes");

    for (int i = 0; i < 64; i++) wbuf[i] = (u8)(0x77 + i);
    bool mem_untainted_before =
        (__atomic_load_n(&tgt->proc_flags, __ATOMIC_ACQUIRE) & PROC_FLAG_DEBUG_TAINTED) == 0;
    TEST_EXPECT_EQ(devproc.write(mem, wbuf, 64, (s64)RW_VA), 64L, "mem write of a stopped target");
    TEST_ASSERT(rw_kva[0] == 0x77, "mem write landed in the target page");
    // The mem path needs authority and a stopped target but NOT the debug-owner
    // slot, so a caller that never attached reaches it -- which is why it carries
    // its own taint stamp rather than leaning on the attach's. SAMPLED here and
    // verdicted after the cleanup below, with this test's other late verdicts: an
    // assertion placed between here and proc_free returns on failure with a
    // LINKED, ALIVE, spoor-open Proc and two pages still held, and the next test
    // to call wait_pid then wedges the entire boot instead of reporting this one
    // red. Measured -- that is exactly what the mem-stamp sabotage leg did.
    bool mem_tainted_after =
        (__atomic_load_n(&tgt->proc_flags, __ATOMIC_ACQUIRE) & PROC_FLAG_DEBUG_TAINTED) != 0;
    TEST_EXPECT_EQ(devproc.read(mem, buf, 64, (s64)GAP_VA), 0L, "mem read of a hole returns 0");

    // Non-owner (a target owned by a different principal; caller has no debug
    // cap) -> refused even while stopped (I-39).
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_DEBUG)),
                "test caller lacks CAP_HOSTOWNER/CAP_DEBUG (the denied case is meaningful)");
    tgt->principal_id = (caller->principal_id == 0x0D0D0D0Du) ? 0x0E0E0E0Eu : 0x0D0D0D0Du;
    TEST_EXPECT_EQ(devproc.read(mem, buf, 64, (s64)RW_VA), (long)-1,
                   "mem read by a non-owner (no CAP_DEBUG) is refused (I-39)");

    // The dump seal refuses the READ direction only (DEBUG-FS-DESIGN 3.2): reading
    // memory is extraction; writing it is control and answers to NOTRACE. LAST,
    // because the bit is one-way. Back to the owner, still stopped, with a positive
    // control one variable away from the seal.
    tgt->principal_id = caller->principal_id;
    long nd_control = devproc.read(mem, buf, 64, (s64)RW_VA);
    proc_seal(tgt, PROC_FLAG_NODUMP);
    long nd_read = devproc.read(mem, buf, 64, (s64)RW_VA);
    for (int i = 0; i < 64; i++) wbuf[i] = (u8)(0x33 + i);
    long nd_write  = devproc.write(mem, wbuf, 64, (s64)RW_VA);
    u8   nd_landed = rw_kva[0];
    spoor_clunk(mem);

    // Cleanup: free MY backing pages (proc_pgtable_destroy leaves leaf data pages
    // to the VMA layer), then the tree via proc_free.
    proc_test_unlink(tgt);
    free_pages(rw_pg, 0);
    free_pages(ro_pg, 0);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    TEST_EXPECT_EQ(nd_control, 64L, "control: the owner's mem read of a stopped target returns the bytes");
    TEST_EXPECT_EQ(nd_read, (long)-1, "the dump seal refuses a mem READ");
    TEST_EXPECT_EQ(nd_write, 64L, "the dump seal does not refuse a mem WRITE -- that is control, NOTRACE's");
    TEST_ASSERT(nd_landed == 0x33, "the mem write to the NODUMP target landed");
    TEST_ASSERT(mem_untainted_before, "premise: the mem target was untainted before the write");
    TEST_ASSERT(mem_tainted_after, "a mem WRITE stamps the debug taint on the target");
}

// 8a-1b-gamma-2: /proc/<pid>/regs + fpregs -- the saved EL0 register frames of a
// STOPPED target's head thread (I-39; DEBUG-FS 4.5). Drives the full devproc
// path on a SYNTHETIC parked thread with a real kstack buffer + a known
// trapframe + FP ctx. The headline check is the SPSR (pstate) privilege guard:
// a regs write applies x0..x30 + sp + pc but NEVER SPSR -- an arbitrary SPSR
// could eret the target to EL1.
// The legs that assert INLINE run in this helper, so a failing TEST_ASSERT returns
// HERE and test_devproc_debug_regs still unlinks its stack-local thread before that
// frame dies. `*done` is set only when every leg passed.
static void debug_regs_inline_legs(struct Proc *tgt, struct Thread *th,
                                   struct exception_context *tf,
                                   struct t_user_regs *ur, struct t_user_regs *wr,
                                   struct t_user_fpregs *uf, bool *done) {
    // --- regs read ---
    struct Spoor *regs = open_pidfile_for(tgt->pid, "regs", 2);   // ORDWR
    TEST_ASSERT(regs != NULL, "open /proc/<pid>/regs");
    TEST_EXPECT_EQ(devproc.read(regs, ur, (long)sizeof(*ur), 0), (long)sizeof(*ur), "regs read: full struct");
    bool m = true; for (int i = 0; i < 31; i++) if (ur->regs[i] != 0x1000ull + (u64)i) m = false;
    TEST_ASSERT(m, "regs read: x0..x30");
    TEST_EXPECT_EQ(ur->sp,     0xDEAD0000ull, "regs read: sp = SP_EL0");
    TEST_EXPECT_EQ(ur->pc,     0xCAFE0000ull, "regs read: pc = ELR_EL1");
    TEST_EXPECT_EQ(ur->pstate, 0x60000000ull, "regs read: pstate = SPSR_EL1");

    // --- regs write: x0..x30 + sp + pc applied; pstate (SPSR) IGNORED (guard) ---
    // Field-wise init (every field is set below), NOT `*wr = *ur`: the 272-byte
    // struct copy lowers to a memcpy call the freestanding kernel does not link
    // under the UBSan (low-opt) build.
    for (int i = 0; i < 31; i++) wr->regs[i] = 0x2000ull + (u64)i;
    wr->sp     = 0xBEEF0000ull;
    wr->pc     = 0xF00D0000ull;
    wr->pstate = 0x00000005ull;   // an EL1h-mode SPSR (M[3:0]=0b0101) -- MUST be ignored
    TEST_EXPECT_EQ(devproc.write(regs, wr, (long)sizeof(*wr), 0), (long)sizeof(*wr), "regs write");
    m = true; for (int i = 0; i < 31; i++) if (tf->regs[i] != 0x2000ull + (u64)i) m = false;
    TEST_ASSERT(m, "regs write applied x0..x30");
    TEST_EXPECT_EQ(tf->sp,  0xBEEF0000ull, "regs write applied sp (SP_EL0)");
    TEST_EXPECT_EQ(tf->elr, 0xF00D0000ull, "regs write applied pc (ELR_EL1)");
    TEST_EXPECT_EQ(tf->spsr, 0x60000000ull,
                   "regs write did NOT change SPSR (the EL1-mode pstate was ignored -- privilege guard)");
    // Control of a target's REGISTERS is control of its image -- point its pc at
    // a store and it writes for you -- so a regs write taints exactly as a mem
    // write does.
    TEST_ASSERT(__atomic_load_n(&tgt->proc_flags, __ATOMIC_ACQUIRE) & PROC_FLAG_DEBUG_TAINTED,
                "a regs WRITE stamps the debug taint on the target");
    spoor_clunk(regs);

    // --- fpregs read + write (all fields; no privilege bits) ---
    struct Spoor *fp = open_pidfile_for(tgt->pid, "fpregs", 2);
    TEST_ASSERT(fp != NULL, "open /proc/<pid>/fpregs");
    TEST_EXPECT_EQ(devproc.read(fp, uf, (long)sizeof(*uf), 0), (long)sizeof(*uf), "fpregs read: full struct");
    m = true; for (int i = 0; i < 512; i++) if (uf->vregs[i] != (u8)(0x30 + (i & 0x3f))) m = false;
    TEST_ASSERT(m, "fpregs read: V0..V31");
    TEST_EXPECT_EQ(uf->fpsr, 0xFEEDFACEu, "fpregs read: fpsr");
    TEST_EXPECT_EQ(uf->fpcr, 0x0BADF00Du, "fpregs read: fpcr");
    uf->fpcr = 0x11112222u;
    TEST_EXPECT_EQ(devproc.write(fp, uf, (long)sizeof(*uf), 0), (long)sizeof(*uf), "fpregs write");
    TEST_EXPECT_EQ(th->ctx.fpcr, 0x11112222u, "fpregs write applied fpcr");
    spoor_clunk(fp);

    // --- not-stopped -> refused (stopped-only) ---
    tgt->debug_stop_req = 0;
    struct Spoor *regs2 = open_pidfile_for(tgt->pid, "regs", 2);
    TEST_EXPECT_EQ(devproc.read(regs2, ur, (long)sizeof(*ur), 0), (long)-1,
                   "regs of a NOT-stopped target is refused");
    spoor_clunk(regs2);
    *done = true;
}

void test_devproc_debug_regs(void) {
    struct Thread *tt = current_thread();
    TEST_ASSERT(tt && tt->proc, "test thread has a proc");
    struct Proc *caller = tt->proc;

    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc regs target");
    tgt->principal_id = caller->principal_id;   // owner -> I-39 authorized
    tgt->state        = PROC_STATE_ALIVE;

    // A real kstack buffer (8 pages) + a synthetic parked head thread.
    struct page *kstk = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!kstk) { tgt->state = PROC_STATE_ZOMBIE; proc_free(tgt); }
    TEST_ASSERT(kstk != NULL, "alloc synthetic kstack");
    u8 *kbase = (u8 *)pa_to_kva(page_to_pa(kstk));

    struct Thread th;
    for (size_t i = 0; i < sizeof(th); i++) ((u8 *)&th)[i] = 0;
    th.magic            = THREAD_MAGIC;
    th.state            = THREAD_SLEEPING;
    th.kstack_base      = kbase;
    th.kstack_size      = THREAD_KSTACK_TOTAL_SIZE;
    th.on_cpu           = false;
    th.rendez_blocked_on = &th.debug_rendez;    // "parked on its own debug_rendez"
    th.next_in_proc     = NULL;
    for (int i = 0; i < 512; i++) th.ctx.fp_v[i] = (u8)(0x30 + (i & 0x3f));
    th.ctx.fpsr = 0xFEEDFACEu;
    th.ctx.fpcr = 0x0BADF00Du;

    // 8a-1c: the trapframe location is NOT a fixed kstack offset -- build_regs
    // reads th.debug_trapframe (what el0_return_stop_check records at the park).
    // A real EL0 thread's frame sits below kstack_top-288; here we place the
    // synthetic frame at the top and point debug_trapframe at it.
    struct exception_context *tf = (struct exception_context *)
        (kbase + THREAD_KSTACK_TOTAL_SIZE - EXCEPTION_CTX_SIZE);
    for (int i = 0; i < 31; i++) tf->regs[i] = 0x1000ull + (u64)i;
    tf->sp   = 0xDEAD0000ull;
    tf->elr  = 0xCAFE0000ull;
    tf->spsr = 0x60000000ull;   // NZCV-ish, EL0t (M[3:0]=0)
    th.debug_trapframe = tf;

    tgt->threads       = &th;
    tgt->debug_stop_req = 1;     // "stopped" (this one parked thread + on_cpu==false)
    proc_test_link(tgt);

    // Zeroed here: the capture legs below reuse them even if a leg above failed.
    struct t_user_regs ur, wr;
    struct t_user_fpregs uf;
    for (size_t i = 0; i < sizeof(ur); i++) { ((u8 *)&ur)[i] = 0; ((u8 *)&wr)[i] = 0; }
    for (size_t i = 0; i < sizeof(uf); i++) ((u8 *)&uf)[i] = 0;
    bool inline_ok = false;
    debug_regs_inline_legs(tgt, &th, tf, &ur, &wr, &uf, &inline_ok);

    // --- 8a-1c holotype HF1 regression: a DYING target is never debug-stopped ---
    // (a) A pending group_exit_msg (SYS_EXIT_GROUP / kill in flight): the parked
    //     head thread still satisfies all_threads_parked, so pre-HF1 the read
    //     returned the full struct -- a torn-ctx read racing the exiting
    //     thread's final sched(). Death wins (debug_stop.tla DeathWinsOverStop).
    tgt->debug_stop_req = 1;
    __atomic_store_n(&tgt->group_exit_msg, "hf1-dying", __ATOMIC_RELEASE);
    struct Spoor *regs3 = open_pidfile_for(tgt->pid, "regs", 2);
    long hf1_msg_rc = regs3 ? devproc.read(regs3, &ur, (long)sizeof(ur), 0) : -2;
    if (regs3) spoor_clunk(regs3);
    __atomic_store_n(&tgt->group_exit_msg, (const char *)NULL, __ATOMIC_RELEASE);

    // (b) The EXITING-head backstop: all-EXITING peers are SKIPPED by
    //     all_threads_parked (vacuously parked, group_exit_msg already NULL
    //     again), so build_regs itself must refuse an EXITING head thread.
    th.state = THREAD_EXITING;
    struct Spoor *regs4 = open_pidfile_for(tgt->pid, "regs", 2);
    long hf1_exiting_rc = regs4 ? devproc.read(regs4, &ur, (long)sizeof(ur), 0) : -2;
    if (regs4) spoor_clunk(regs4);
    th.state = THREAD_SLEEPING;

    // The dump seal refuses regs/fpregs READS (extraction) and neither WRITE
    // (control, NOTRACE's). LAST, because the bit is one-way and every leg above
    // would otherwise be refused by the seal instead of by the guard it names. HF1's
    // perturbations are undone above (thread SLEEPING, no group exit); the target is
    // stopped, and each read has a positive control one variable away.
    tgt->debug_stop_req = 1;
    struct Spoor *nd_r = open_pidfile_for(tgt->pid, "regs", 2);
    struct Spoor *nd_f = open_pidfile_for(tgt->pid, "fpregs", 2);
    long nd_regs_ctl = nd_r ? devproc.read(nd_r, &ur, (long)sizeof(ur), 0) : -2;
    long nd_fp_ctl   = nd_f ? devproc.read(nd_f, &uf, (long)sizeof(uf), 0) : -2;
    proc_seal(tgt, PROC_FLAG_NODUMP);
    long nd_regs_rd  = nd_r ? devproc.read(nd_r, &ur, (long)sizeof(ur), 0) : -2;
    long nd_fp_rd    = nd_f ? devproc.read(nd_f, &uf, (long)sizeof(uf), 0) : -2;
    long nd_regs_wr  = nd_r ? devproc.write(nd_r, &wr, (long)sizeof(wr), 0) : -2;
    long nd_fp_wr    = nd_f ? devproc.write(nd_f, &uf, (long)sizeof(uf), 0) : -2;
    if (nd_r) spoor_clunk(nd_r);
    if (nd_f) spoor_clunk(nd_f);

    // Cleanup BEFORE the HF1 asserts: unlink + drop the synthetic (stack-local)
    // thread BEFORE the frame dies, free the kstack, then the Proc. TEST_ASSERT
    // returns on failure -- asserting first would leave tgt linked with a
    // dangling tgt->threads (the kregs lesson).
    proc_test_unlink(tgt);
    tgt->threads = NULL;
    free_pages(kstk, THREAD_KSTACK_TOTAL_ORDER);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);
    if (!inline_ok) return;   // its failure is already recorded; the captures are moot

    TEST_EXPECT_EQ(hf1_msg_rc, (long)-1,
                   "regs of a group-terminating target is refused (HF1)");
    TEST_EXPECT_EQ(hf1_exiting_rc, (long)-1,
                   "regs of an EXITING head thread is refused (HF1 backstop)");
    TEST_EXPECT_EQ(nd_regs_ctl, (long)sizeof(ur), "control: regs reads before the seal");
    TEST_EXPECT_EQ(nd_fp_ctl,   (long)sizeof(uf), "control: fpregs reads before the seal");
    TEST_EXPECT_EQ(nd_regs_rd,  (long)-1, "the dump seal refuses a regs READ");
    TEST_EXPECT_EQ(nd_fp_rd,    (long)-1, "the dump seal refuses an fpregs READ");
    TEST_EXPECT_EQ(nd_regs_wr,  (long)sizeof(wr), "the dump seal does not refuse a regs WRITE (control)");
    TEST_EXPECT_EQ(nd_fp_wr,    (long)sizeof(uf), "the dump seal does not refuse an fpregs WRITE (control)");
}

// 8a-1b-gamma-3: /proc/<pid>/{kregs,kstack,wait} -- the kernel-side inspection +
// stop-notification files of a STOPPED target's head thread (I-39; DEBUG-FS
// 4.5/4.6). Drives the full devproc path on a SYNTHETIC parked thread with a
// real kstack, a known t->ctx GP frame, and a hand-built fp-chain in the usable
// kstack region. The kstack walk reuses the additive halls_walk_kernel_frames
// primitive (the dying-machine dump path stays byte-unchanged). The blocking
// wait path (block-until-a-real-thread-parks) is the in-guest E2E (8a-1c); here
// a target that is ALREADY stopped returns immediately (level-triggered).
//
// STRUCTURE (deliberate): capture every result into locals, then CLEAN UP the
// linked target BEFORE any content assert. TEST_ASSERT `return`s on failure, so
// an assert after proc_test_link would skip the cleanup and leave `tgt` linked
// with a DANGLING tgt->threads (this stack-local `th`) -- a later proc-table
// walk then derefs a dead stack frame (the mystery downstream hang this test
// itself first exposed). Cleanup-before-assert makes a failure report cleanly.
void test_devproc_debug_kregs_kstack_wait(void) {
    struct Thread *tt = current_thread();
    TEST_ASSERT(tt && tt->proc, "test thread has a proc");
    struct Proc *caller = tt->proc;
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_DEBUG)),
                "test caller lacks CAP_HOSTOWNER/CAP_DEBUG (the non-owner deny case is meaningful)");

    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc debug target");
    tgt->principal_id = caller->principal_id;   // owner -> I-39 authorized
    tgt->state        = PROC_STATE_ALIVE;

    struct page *kstk = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!kstk) { tgt->state = PROC_STATE_ZOMBIE; proc_free(tgt); }
    TEST_ASSERT(kstk != NULL, "alloc synthetic kstack");
    u8 *kbase = (u8 *)pa_to_kva(page_to_pa(kstk));

    // Hand-built kernel fp-chain in the USABLE kstack region (above the guard):
    //   fp0 -> fp1 -> 0(sentinel).  Frame record = [fp]=next_fp, [fp+8]=saved LR.
    // Strictly-increasing addresses (fp0 < fp1) so halls_fp_is_sane accepts them.
    u8 *fp0 = kbase + THREAD_KSTACK_TOTAL_SIZE - 256;   // 16-aligned, in [guard, top)
    u8 *fp1 = kbase + THREAD_KSTACK_TOTAL_SIZE - 128;
    *(volatile u64 *)fp0        = (u64)(uintptr_t)fp1;   // next frame
    *(volatile u64 *)(fp0 + 8)  = 0x22220000ull;        // LR0 (frame #1)
    *(volatile u64 *)fp1        = 0;                     // sentinel -> the walk stops
    *(volatile u64 *)(fp1 + 8)  = 0x33330000ull;        // LR1 (frame #2)

    struct Thread th;
    for (size_t i = 0; i < sizeof(th); i++) ((u8 *)&th)[i] = 0;
    th.magic             = THREAD_MAGIC;
    th.state             = THREAD_SLEEPING;
    th.kstack_base       = kbase;
    th.kstack_size       = THREAD_KSTACK_TOTAL_SIZE;
    th.on_cpu            = false;
    th.rendez_blocked_on = &th.debug_rendez;    // "parked on its own debug_rendez"
    th.next_in_proc      = NULL;
    // Kernel-side saved GP frame (t->ctx): x19..x28 + fp/lr/sp + TLS. fp = fp0
    // (the walk start), lr = 0x11110000 (the walk's frame #0 PC). x-values are
    // 0xC200 + i (sequential, distinct from sp=0xC096 / tpidr=0xC104).
    th.ctx.x19 = 0xC200ull; th.ctx.x20 = 0xC201ull; th.ctx.x21 = 0xC202ull;
    th.ctx.x22 = 0xC203ull; th.ctx.x23 = 0xC204ull; th.ctx.x24 = 0xC205ull;
    th.ctx.x25 = 0xC206ull; th.ctx.x26 = 0xC207ull; th.ctx.x27 = 0xC208ull;
    th.ctx.x28 = 0xC209ull;
    th.ctx.fp        = (u64)(uintptr_t)fp0;
    th.ctx.lr        = 0x11110000ull;
    th.ctx.sp        = 0xC096ull;
    th.ctx.tpidr_el0 = 0xC104ull;
    th.ctx.ttbr0     = 0xDEADBEEFull;   // MUST NOT appear in kregs (info-leak omission)

    tgt->threads        = &th;
    tgt->debug_stop_req = 1;            // "stopped" (one parked thread + on_cpu==false)
    proc_test_link(tgt);

    // --- Capture every result while linked+stopped (NO content asserts yet) ---
    // Pre-filled, so a field the kernel withholds reads as a WRITTEN zero rather
    // than a byte the read never touched.
    struct t_kernel_regs kr, kr_cap;
    for (size_t i = 0; i < sizeof(kr); i++) { ((u8 *)&kr)[i] = 0xA5; ((u8 *)&kr_cap)[i] = 0xA5; }
    long kregs_rlen = -999, kregs_wlen = -999, kregs_cap_rlen = -999;
    struct Spoor *kregs = open_pidfile_for(tgt->pid, "kregs", 0);   // OREAD
    if (kregs) {
        kregs_rlen = devproc.read(kregs, &kr, (long)sizeof(kr), 0);   // the OWNER axis
        kregs_wlen = devproc.write(kregs, &kr, (long)sizeof(kr), 0);   // RO -> -1
        // I-16: the raw kernel frame belongs to the CAP tier, like kstack's raw
        // addresses -- the one variable between this read and the one above.
        __atomic_fetch_or(&caller->caps, CAP_DEBUG, __ATOMIC_RELEASE);
        kregs_cap_rlen = devproc.read(kregs, &kr_cap, (long)sizeof(kr_cap), 0);
        __atomic_fetch_and(&caller->caps, ~(u64)CAP_DEBUG, __ATOMIC_RELEASE);
        spoor_clunk(kregs);
    }

    // 8b F1 (I-16): the raw slid frame addrs are CAP-tier only; this walk-frame
    // verification reads in the CAP_DEBUG tier to see them (the owner axis gets the
    // KASLR-independent symbolic form, covered by devproc.debug_kstack_settled).
    char sbuf[512]; for (size_t i = 0; i < sizeof(sbuf); i++) sbuf[i] = 0;
    long slen = -999;
    __atomic_fetch_or(&caller->caps, CAP_DEBUG, __ATOMIC_RELEASE);
    struct Spoor *ks = open_pidfile_for(tgt->pid, "kstack", 0);
    if (ks) { slen = devproc.read(ks, sbuf, (long)sizeof(sbuf), 0); spoor_clunk(ks); }
    __atomic_fetch_and(&caller->caps, ~(u64)CAP_DEBUG, __ATOMIC_RELEASE);

    // The dump seal, after every unsealed kregs/kstack read (it is one-way): kregs
    // carries tpidr_el0, something the target HOLDS, so a sealed target refuses it;
    // kstack is the kernel's own state and still answers -- the one-variable
    // control. The wait legs below run on the sealed target on purpose: wait is
    // control, and the seal must not change it.
    proc_seal(tgt, PROC_FLAG_NODUMP);
    long nd_kregs = -999, nd_kstack = -999;
    struct Spoor *kregs_nd = open_pidfile_for(tgt->pid, "kregs", 0);
    if (kregs_nd) {
        struct t_kernel_regs junk;
        nd_kregs = devproc.read(kregs_nd, &junk, (long)sizeof(junk), 0);
        spoor_clunk(kregs_nd);
    }
    struct Spoor *ks_nd = open_pidfile_for(tgt->pid, "kstack", 0);
    if (ks_nd) {
        char junk[256];
        nd_kstack = devproc.read(ks_nd, junk, (long)sizeof(junk), 0);
        spoor_clunk(ks_nd);
    }

    char wbuf[16], ebuf[16]; long wl = -999, wl_nonowner = -999, el = -999;
    for (size_t i = 0; i < sizeof(wbuf); i++) { wbuf[i] = 0; ebuf[i] = 0; }
    struct Spoor *w = open_pidfile_for(tgt->pid, "wait", 0);
    if (w) {
        wl = devproc.read(w, wbuf, (long)sizeof(wbuf), 0);   // stopped -> "stopped\n"
        // Non-owner -> denied (I-39), even while stopped (immediate, no block).
        u32 saved_pid = tgt->principal_id;
        tgt->principal_id = (caller->principal_id == 0x0D0D0D0Du) ? 0x0E0E0E0Eu : 0x0D0D0D0Du;
        char junk[16];
        wl_nonowner = devproc.read(w, junk, (long)sizeof(junk), 0);
        tgt->principal_id = saved_pid;
        // Exiting target -> "exited" (the debugger unblocks on the target's death).
        tgt->state = PROC_STATE_ZOMBIE;
        el = devproc.read(w, ebuf, (long)sizeof(ebuf), 0);
        spoor_clunk(w);
    }

    // --- Cleanup FIRST: unlink the target + drop the stack-local thread before
    //     any content assert can `return` and strand a linked Proc. ---
    proc_test_unlink(tgt);
    tgt->threads = NULL;
    free_pages(kstk, THREAD_KSTACK_TOTAL_ORDER);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    // --- Now assert on the captured results (safe: nothing linked) ---
    // kregs, OWNER axis: the EL0 TLS base only. The kernel half is raw slid state
    // (ctx.lr is the return into sched), i.e. the KASLR slide (I-16).
    TEST_EXPECT_EQ(kregs_rlen, (long)sizeof(kr), "kregs read (owner axis): full struct");
    bool withheld = true;
    for (int i = 0; i < 10; i++) if (kr.x[i] != 0) withheld = false;
    TEST_ASSERT(withheld, "kregs (owner axis): x19..x28 withheld (I-16)");
    TEST_EXPECT_EQ(kr.fp,        0ull,      "kregs (owner axis): fp withheld (I-16)");
    TEST_EXPECT_EQ(kr.lr,        0ull,      "kregs (owner axis): lr withheld -- it is the KASLR slide (I-16)");
    TEST_EXPECT_EQ(kr.sp,        0ull,      "kregs (owner axis): sp withheld (I-16)");
    TEST_EXPECT_EQ(kr.tpidr_el0, 0xC104ull, "kregs (owner axis): tpidr_el0 still delivered (Delve's g)");
    // kregs, CAP_DEBUG tier: the whole saved frame.
    TEST_EXPECT_EQ(kregs_cap_rlen, (long)sizeof(kr_cap), "kregs read (CAP tier): full struct");
    bool m = true;
    for (int i = 0; i < 10; i++) if (kr_cap.x[i] != 0xC200ull + (u64)i) m = false;
    TEST_ASSERT(m, "kregs (CAP tier): x19..x28 (callee-saved)");
    TEST_EXPECT_EQ(kr_cap.fp,        (u64)(uintptr_t)fp0, "kregs (CAP tier): fp = ctx.fp (kstack-walk start)");
    TEST_EXPECT_EQ(kr_cap.lr,        0x11110000ull,       "kregs (CAP tier): lr = ctx.lr");
    TEST_EXPECT_EQ(kr_cap.sp,        0xC096ull,           "kregs (CAP tier): sp");
    TEST_EXPECT_EQ(kr_cap.tpidr_el0, 0xC104ull,           "kregs (CAP tier): tpidr_el0");
    TEST_EXPECT_EQ(kregs_wlen,   (long)-1,            "kregs is RO (write refused)");
    TEST_EXPECT_EQ(nd_kregs, (long)-1, "the dump seal refuses a kregs READ (it carries tpidr_el0)");
    TEST_ASSERT(nd_kstack > 0, "control: kstack is the kernel's own state -- it still reads when sealed");

    // kstack: the symbolized kernel fp-chain walk. Three frames: #0 = ctx.lr
    // (0x11110000), #1 = LR0 (0x22220000), #2 = LR1 (0x33330000). KASLR is off in
    // the test kernel (koff=0) + the addresses are below any slide, so link ==
    // runtime addr. The sentinel (fp2=0) stops the walk at 3 frames.
    TEST_ASSERT(slen > 0,                              "kstack read produced text");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "#0"),         "kstack: frame #0 present");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "0x11110000"), "kstack: frame #0 addr = ctx.lr");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "#1"),         "kstack: frame #1 present");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "0x22220000"), "kstack: frame #1 addr = LR0");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "#2"),         "kstack: frame #2 present");
    TEST_ASSERT(contains(sbuf, (size_t)slen, "0x33330000"), "kstack: frame #2 addr = LR1");
    TEST_ASSERT(!contains(sbuf, (size_t)slen, "#3"),        "kstack: the sentinel stopped the walk at 3 frames");

    // wait: stopped / denied / exited (all level-triggered immediate returns).
    TEST_EXPECT_EQ(wl, 8L, "wait on a stopped target returns 'stopped\\n'");
    TEST_ASSERT(wl == 8 && contains(wbuf, (size_t)wl, "stopped"), "wait status = stopped");
    TEST_EXPECT_EQ(wl_nonowner, (long)-1, "wait by a non-owner (no CAP_DEBUG) is refused (I-39)");
    TEST_EXPECT_EQ(el, 7L, "wait on an exiting target returns 'exited\\n'");
    TEST_ASSERT(el == 7 && contains(ebuf, (size_t)el, "exited"), "wait status = exited");
}

// 8b: the SETTLED-thread kstack inspect (DEBUG-FS 5b). Reads a thread's KERNEL
// stack while it is BLOCKED in a syscall (on_cpu==false, debug_stop_req==0)
// WITHOUT a debug-stop -- the Linux /proc/<pid>/stack tier. The 8a fully_stopped
// gate rejected this (debug_stop_req==0 -> -1); the 8b relaxation walks it. Also:
// a running head (on_cpu==true) reports "<running>", and I-39 is preserved.
void test_devproc_debug_kstack_settled(void) {
    struct Thread *tt = current_thread();
    TEST_ASSERT(tt && tt->proc, "test thread has a proc");
    struct Proc *caller = tt->proc;
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_DEBUG)),
                "test caller lacks CAP_HOSTOWNER/CAP_DEBUG (the non-owner deny case is meaningful)");

    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc debug target");
    tgt->principal_id = caller->principal_id;   // owner -> I-39 authorized
    tgt->state        = PROC_STATE_ALIVE;

    struct page *kstk = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!kstk) { tgt->state = PROC_STATE_ZOMBIE; proc_free(tgt); }
    TEST_ASSERT(kstk != NULL, "alloc synthetic kstack");
    u8 *kbase = (u8 *)pa_to_kva(page_to_pa(kstk));

    // Hand-built kernel fp-chain (fp0 -> fp1 -> sentinel), the gamma-3 shape.
    u8 *fp0 = kbase + THREAD_KSTACK_TOTAL_SIZE - 256;
    u8 *fp1 = kbase + THREAD_KSTACK_TOTAL_SIZE - 128;
    *(volatile u64 *)fp0        = (u64)(uintptr_t)fp1;
    *(volatile u64 *)(fp0 + 8)  = 0x22220000ull;
    *(volatile u64 *)fp1        = 0;
    *(volatile u64 *)(fp1 + 8)  = 0x33330000ull;

    struct Thread th;
    for (size_t i = 0; i < sizeof(th); i++) ((u8 *)&th)[i] = 0;
    th.magic             = THREAD_MAGIC;
    th.state             = THREAD_SLEEPING;
    th.kstack_base       = kbase;
    th.kstack_size       = THREAD_KSTACK_TOTAL_SIZE;
    th.on_cpu            = false;
    // Blocked on a NON-debug rendez (a real syscall block) -- the 8b case the
    // fully_stopped gate rejected. The relaxed read never consults this field.
    th.rendez_blocked_on = NULL;
    th.next_in_proc      = NULL;
    th.ctx.fp = (u64)(uintptr_t)fp0;
    th.ctx.lr = 0x11110000ull;

    tgt->threads        = &th;
    tgt->debug_stop_req = 0;            // NOT debug-stopped -- the 8b relaxation
    proc_test_link(tgt);

    // --- Capture every result (NO content asserts yet; cleanup precedes asserts) ---
    // (a) settled + NOT debug-stopped, OWNER axis (no cap) -> the 8b walk in the
    //     SYMBOLIC form (F1/I-16: no raw slid addr -> no koff leak to the owner).
    char sbuf[512]; for (size_t i = 0; i < sizeof(sbuf); i++) sbuf[i] = 0;
    long slen_settled = -999;
    struct Spoor *ks = open_pidfile_for(tgt->pid, "kstack", 0);
    if (ks) { slen_settled = devproc.read(ks, sbuf, (long)sizeof(sbuf), 0); spoor_clunk(ks); }

    // (a2) the SAME settled read with CAP_DEBUG -> the RAW form (the debugger tier
    //      sees the slid addr, exactly as it reads /ctl/kernel-base).
    char cbuf[512]; for (size_t i = 0; i < sizeof(cbuf); i++) cbuf[i] = 0;
    long slen_cap = -999;
    __atomic_fetch_or(&caller->caps, CAP_DEBUG, __ATOMIC_RELEASE);
    struct Spoor *kc = open_pidfile_for(tgt->pid, "kstack", 0);
    if (kc) { slen_cap = devproc.read(kc, cbuf, (long)sizeof(cbuf), 0); spoor_clunk(kc); }
    __atomic_fetch_and(&caller->caps, ~(u64)CAP_DEBUG, __ATOMIC_RELEASE);

    // (b) a running head -> "<running>" (no walk of a stale ctx).
    char rbuf[512]; for (size_t i = 0; i < sizeof(rbuf); i++) rbuf[i] = 0;
    long slen_running = -999;
    th.on_cpu = true;
    struct Spoor *kr = open_pidfile_for(tgt->pid, "kstack", 0);
    if (kr) { slen_running = devproc.read(kr, rbuf, (long)sizeof(rbuf), 0); spoor_clunk(kr); }
    th.on_cpu = false;

    // (c) I-39: a non-owner (no CAP_DEBUG) is refused even for the read-only inspect.
    char jbuf[64]; long slen_nonowner = -999;
    u32 saved_pid = tgt->principal_id;
    tgt->principal_id = (caller->principal_id == 0x0D0D0D0Du) ? 0x0E0E0E0Eu : 0x0D0D0D0Du;
    struct Spoor *kn = open_pidfile_for(tgt->pid, "kstack", 0);
    if (kn) { slen_nonowner = devproc.read(kn, jbuf, (long)sizeof(jbuf), 0); spoor_clunk(kn); }
    tgt->principal_id = saved_pid;

    // (d) F3: an EXITING head -> empty (never walk a head running its exit path).
    char dbuf[64]; for (size_t i = 0; i < sizeof(dbuf); i++) dbuf[i] = 0;
    long slen_exiting = -999;
    th.state = THREAD_EXITING;
    struct Spoor *kx = open_pidfile_for(tgt->pid, "kstack", 0);
    if (kx) { slen_exiting = devproc.read(kx, dbuf, (long)sizeof(dbuf), 0); spoor_clunk(kx); }
    th.state = THREAD_SLEEPING;   // restore (cleanup nulls threads next anyway)

    // --- Cleanup FIRST (before any content assert can return + strand the link) ---
    proc_test_unlink(tgt);
    tgt->threads = NULL;
    free_pages(kstk, THREAD_KSTACK_TOTAL_ORDER);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    // --- Assert (safe: nothing linked) ---
    // The 8b headline: a settled-but-NOT-stopped thread's kernel stack walks.
    // Revert-probe: restoring the fully_stopped gate makes slen_settled == -1
    // (debug_stop_req==0 -> devproc_target_fully_stopped false).
    TEST_ASSERT(slen_settled > 0,                              "8b: settled (not debug-stopped) kstack walks");
    TEST_ASSERT(contains(sbuf, (size_t)slen_settled, "#0"),         "8b: frame #0 present");
    // F1 (I-16): the OWNER axis gets NO raw slid addr (no koff leak). Revert-probe:
    // dropping the raw-gate (always-raw) makes this CONTAIN "0x11110000" -> FAIL.
    TEST_ASSERT(!contains(sbuf, (size_t)slen_settled, "0x11110000"),
                "8b F1: owner axis -> no raw slid addr (koff protected)");
    // The CAP_DEBUG tier gets the raw slid addr (the debugger view; frame #0 = ctx.lr).
    TEST_ASSERT(slen_cap > 0,                                  "8b: CAP-tier settled kstack walks");
    TEST_ASSERT(contains(cbuf, (size_t)slen_cap, "0x11110000"),
                "8b F1: CAP tier -> raw slid addr (frame #0 = ctx.lr)");
    // A running head reports the marker (its live regs are in HW, not ctx).
    TEST_ASSERT(slen_running > 0,                             "8b: running head produced text");
    TEST_ASSERT(contains(rbuf, (size_t)slen_running, "running"), "8b: running head reports <running>");
    // I-39 authorization preserved for the inspect tier.
    TEST_EXPECT_EQ(slen_nonowner, (long)-1, "8b: non-owner (no CAP_DEBUG) inspect refused (I-39)");
    // F3: an EXITING head -> empty (the death-adjacent guard; the relaxed gate no
    // longer rejects a dying target, so the format-time EXITING guard is load-bearing).
    TEST_EXPECT_EQ(slen_exiting, 0L, "8b F3: EXITING head -> empty read (never walk a dying head)");
}

// =============================================================================
// VIVARIUM V-4a-0: /proc/<pid>/exe -- the executable's namespace name.
// =============================================================================
//
// The source the diorama re-presents as Linux's /proc/self/exe (VIVARIUM.md
// section 6.3 Tier 1). Before this, NOTHING in the system knew what a running
// program was CALLED: struct Proc carried no name, the Image cache is qid-keyed
// and the text Burrow anonymous, and format_cmdline is a stub -- so the file
// could not be rendered from any existing surface at all.
//
// Drives the real devproc read path on a Proc with a real #66 Path, and pins
// the four properties a consumer depends on:
//   (a) the bytes are the path EXACTLY -- no NUL, no newline (readlink("/proc/
//       self/exe") yields a bare path; a terminator lands inside every caller's
//       buffer);
//   (b) no recorded name -> an EMPTY read, never -1 (kproc + the blob-loaded
//       init /joey legitimately have none -- I-33 fail-soft);
//   (c) the offset window matches a full read (the 9P/pread path);
//   (d) proc_set_exe_path is ref-new-BEFORE-unref-old, so the self-assignment
//       the rfork inherit reaches (a child re-execing the parent's own binary)
//       cannot free the Path it is about to store.
void test_devproc_read_exe(void) {
    u64 alloc0 = path_total_allocated();
    u64 freed0 = path_total_freed();

    // (b) FIRST, on kproc (pid 0): blob-loaded, so exe_path is NULL. An empty
    //     read, NOT an error -- the file exists and is honest about not knowing.
    struct Spoor *ke = open_pidfile_for(0, "exe", 0);
    TEST_ASSERT(ke != NULL, "open /proc/0/exe (the file exists for a nameless Proc)");
    char kbuf[64];
    long kn = devproc.read(ke, kbuf, (long)sizeof(kbuf), 0);
    spoor_clunk(ke);
    TEST_EXPECT_EQ(kn, 0L, "V-4a-0: no recorded exe -> empty read (not -1)");

    // A target with a real resolved name, built exactly as stalk builds one.
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc exe target");
    tgt->state = PROC_STATE_ALIVE;
    proc_test_link(tgt);           // so the /proc/<pid> walk resolves it

    struct Path *root = path_make_root();
    TEST_ASSERT(root != NULL, "path_make_root");
    struct Path *bin = path_addelem(root, "bin", 3);
    TEST_ASSERT(bin != NULL, "path_addelem /bin");
    struct Path *prog = path_addelem(bin, "diorama", 7);
    TEST_ASSERT(prog != NULL, "path_addelem /bin/diorama");
    path_unref(root);
    path_unref(bin);
    // prog now holds the sole ref; proc_set_exe_path takes its own.
    proc_set_exe_path(tgt, prog);
    path_unref(prog);              // the Proc is the only holder now

    // (d) Self-assignment must NOT free the Path (ref-new-before-unref-old).
    //     A unref-first implementation drops the last ref here and then refs
    //     freed storage -- the read below would return garbage or extinct.
    proc_set_exe_path(tgt, tgt->exe_path);

    char buf[64];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (char)0xAA;
    struct Spoor *e = open_pidfile_for(tgt->pid, "exe", 0);
    long n = -999;
    if (e) { n = devproc.read(e, buf, (long)sizeof(buf), 0); spoor_clunk(e); }

    // (c) the offset window: bytes [3, 3+len) of a fresh read must match.
    char wbuf[64];
    for (size_t i = 0; i < sizeof(wbuf); i++) wbuf[i] = (char)0x55;
    long wn = -999;
    struct Spoor *e2 = open_pidfile_for(tgt->pid, "exe", 0);
    if (e2) { wn = devproc.read(e2, wbuf, 4, 3); spoor_clunk(e2); }

    // Cleanup BEFORE any assert can return and strand the table link.
    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    // --- Assert (safe: nothing linked) ---
    TEST_EXPECT_EQ(n, 12L, "V-4a-0: /proc/<pid>/exe reads the whole path");
    // (a) the exact bytes, no terminator. Revert-probe: appending a '\n' or a
    //     NUL in format_exe makes n == 13 and this comparison fail.
    TEST_ASSERT(n == 12 && buf[0] == '/' && buf[1] == 'b' && buf[2] == 'i' &&
                buf[3] == 'n' && buf[4] == '/' && buf[5] == 'd' && buf[6] == 'i' &&
                buf[7] == 'o' && buf[8] == 'r' && buf[9] == 'a' && buf[10] == 'm' &&
                buf[11] == 'a',
                "V-4a-0: exe is exactly \"/bin/diorama\" -- no NUL, no newline");
    TEST_ASSERT(buf[12] == (char)0xAA, "V-4a-0: nothing written past the path");
    TEST_EXPECT_EQ(wn, 4L, "V-4a-0: offset read returns the window");
    TEST_ASSERT(wn == 4 && wbuf[0] == 'n' && wbuf[1] == '/' && wbuf[2] == 'd' &&
                wbuf[3] == 'i',
                "V-4a-0: exe[3..7) == \"n/di\" (offset window matches)");

    // (d) again, from the other side: every Path this test allocated is freed.
    //     A leaked ref (proc_free forgetting path_unref) or a double-free (the
    //     self-assign order bug) both break this equality.
    TEST_EXPECT_EQ(path_total_allocated() - alloc0,
                   path_total_freed() - freed0,
                   "V-4a-0: exe Path refs balance (no leak, no double-free)");
}

// VIVARIUM V-4b-1: /proc/<pid>/cwd -- the Territory's cwd, the source the diorama
// re-presents as Linux's /proc/self/cwd. Unlike exe this is never empty for a
// live Proc: a NULL dot_path means "/" (territory_getdot's contract), so the
// Linux consumer always gets a usable path. Bare bytes, like exe.
// VIVARIUM V-4b-2: /proc/<pid>/maps -- the VMA table the diorama re-presents as
// Linux's /proc/self/maps. Builds a synthetic address space with one VMA of each
// shape the renderer discriminates (a plain anon mapping, an anon mapping AT the
// exec stack base, and a guard VMA with no backing Burrow), then reads the file
// and checks each row. Revert-probe anchors are noted per assert.
void test_devproc_maps(void) {
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc maps target");
    tgt->state = PROC_STATE_ALIVE;
    proc_test_link(tgt);

    // (1) a plain RW anon mapping, well clear of the exec layout constants.
    const u64 plain_va = 0x0000000010000000ull;
    struct Burrow *b1 = burrow_create_anon(PAGE_SIZE, false);
    int rc1 = b1 ? burrow_map(tgt, b1, plain_va, PAGE_SIZE, VMA_PROT_RW) : -1;
    if (b1) burrow_unref(b1);            // the mapping ref keeps it alive

    // (2) an anon mapping exactly at the stack base -> role "stack".
    struct Burrow *b2 = burrow_create_anon(PAGE_SIZE, false);
    int rc2 = b2 ? burrow_map(tgt, b2, EXEC_USER_STACK_BASE, PAGE_SIZE, VMA_PROT_RW) : -1;
    if (b2) burrow_unref(b2);

    // (3) a guard VMA: no Burrow, prot == 0 -> type "none", role "guard".
    struct Vma *g = vma_alloc_guard(EXEC_USER_STACK_GUARD_BASE, EXEC_USER_STACK_BASE);
    int rc3 = -1;
    if (g) {
        rc3 = vma_insert(tgt, g);
        if (rc3 != 0) vma_free(g);
    }

    char buf[1024];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (char)0xAA;
    long n = -999;
    struct Spoor *c = open_pidfile_for(tgt->pid, "maps", 0);
    if (c) { n = devproc.read(c, buf, (long)sizeof(buf) - 1, 0); spoor_clunk(c); }
    if (n > 0) buf[n] = '\0'; else buf[0] = '\0';

    // Cleanup BEFORE any assert can return and strand the table link. proc_free
    // runs vma_drain, which releases every mapping ref -- and since we already
    // dropped our handle refs, that frees both Burrows (the I-7 dual count).
    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    // --- Assert (safe: nothing linked) ---
    TEST_EXPECT_EQ((long)rc1, 0L, "V-4b-2: mapped the plain anon VMA");
    TEST_EXPECT_EQ((long)rc2, 0L, "V-4b-2: mapped the stack-base anon VMA");
    TEST_EXPECT_EQ((long)rc3, 0L, "V-4b-2: inserted the guard VMA");
    TEST_ASSERT(n > 0, "V-4b-2: /proc/<pid>/maps reads non-empty");

    // The header names the columns, so a consumer can split without a schema.
    TEST_ASSERT(contains(buf, (size_t)n, "start-end perms off type file role\n"),
                "V-4b-2: maps leads with the column header");

    // The plain mapping: RW, private, anon-backed, no file identity, no role.
    // Revert-probe: flip the perms order, or emit `s` unconditionally, and the
    // "rw-p" here fails.
    TEST_ASSERT(contains(buf, (size_t)n, "0x10000000-0x10001000 rw-p 0x0 anon - -\n"),
                "V-4b-2: the plain anon row is exactly rw-p/anon/-/-");

    // The stack mapping: same shape, but the role column names it. Revert-probe:
    // drop the EXEC_USER_STACK_BASE arm from maps_role_name and this fails while
    // the row above still passes -- so the two are independently pinned.
    TEST_ASSERT(contains(buf, (size_t)n, " rw-p 0x0 anon - stack\n"),
                "V-4b-2: the VMA at the exec stack base carries role \"stack\"");

    // The guard VMA: prot == 0 renders "---p", a NULL Burrow renders type "none"
    // AND role "guard" -- the two are separate arms, so this pins both.
    TEST_ASSERT(contains(buf, (size_t)n, " ---p 0x0 none - guard\n"),
                "V-4b-2: a guard VMA is ---p / none / guard");

    // Ascending order is vma_insert's contract; the renderer must not reorder.
    // The guard sits one page below the stack, so its row must precede it.
    long guard_at = index_of(buf, (size_t)n, " guard\n");
    long stack_at = index_of(buf, (size_t)n, " stack\n");
    TEST_ASSERT(guard_at >= 0 && stack_at >= 0 && guard_at < stack_at,
                "V-4b-2: rows are emitted in ascending-VA order");
}

// VIVARIUM V-4b-6: /proc/<pid>/environ -- the gate, the wiring, the 0400 mode,
// and the short-read-not-truncation property of the per-call clamp.
//
// The gate's axes, driven through environ's own predicate with synthetic (caller,
// target) pairs -- the prowl-5-F4 precedent. The call site is pinned separately, end
// to end, by test_devproc_dump_seal_disclosure: the in-kernel runner holds NO
// CAP_HOSTOWNER (CAP_ALL excludes every elevation-only cap), so a cross-principal
// environ read is reachable there and must read -1.
void test_devproc_environ(void) {
    // --- the gate ----------------------------------------------------------
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");
    target->principal_id = 0xA11CEu;

    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    TEST_ASSERT(!devproc_extract_authorized(caller, target),
                "V-4b-6: a non-owner without caps cannot read a peer's environ");
    caller->principal_id = 0xA11CEu;
    TEST_ASSERT(devproc_extract_authorized(caller, target),
                "V-4b-6: the owner can");
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_HOSTOWNER;
    TEST_ASSERT(devproc_extract_authorized(caller, target),
                "V-4b-6: CAP_HOSTOWNER can");
    // CAP_DEBUG is deliberately NOT an axis: environ is an info file, and a
    // debugger's authority to stop a Proc is a different grant from a reader's.
    caller->caps = CAP_DEBUG;
    TEST_ASSERT(!devproc_extract_authorized(caller, target),
                "V-4b-6: CAP_DEBUG alone is not an environ axis");
    caller->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    target->state = PROC_STATE_ZOMBIE;
    proc_free(target);

    // --- the live read -----------------------------------------------------
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "proc_alloc environ target");
    tgt->principal_id = current_thread()->proc->principal_id;   // owner -> allowed
    tgt->state        = PROC_STATE_ALIVE;
    proc_test_link(tgt);

    u64 a = env_create(tgt, "A", 1);
    u64 b = env_create(tgt, "BB", 2);
    TEST_ASSERT(a != 0 && b != 0, "populate the target's env");
    TEST_EXPECT_EQ(env_write(tgt, a, 0, "1", 1),  1L, "A=1");
    TEST_EXPECT_EQ(env_write(tgt, b, 0, "22", 2), 2L, "BB=22");

    struct Spoor *c = open_pidfile_for(tgt->pid, "environ", 0);
    TEST_ASSERT(c != NULL, "walk + open /proc/<pid>/environ");

    struct t_stat st;
    TEST_EXPECT_EQ(devproc.stat_native(c, &st), 0, "stat_native(environ) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0400u),
                   "V-4b-6: environ is 0400 -- gated, unlike its 0444 siblings");

    char buf[64];
    long n = devproc.read(c, buf, (long)sizeof(buf), 0);
    const char want[] = "A=1\0BB=22\0";
    const long wlen = (long)sizeof(want) - 1;
    TEST_EXPECT_EQ(n, wlen, "V-4b-6: the read is wired to the env render");
    bool same = (n == wlen);
    for (long i = 0; same && i < wlen; i++) same = (buf[i] == want[i]);
    TEST_ASSERT(same, "V-4b-6: /proc/<pid>/environ serves the NUL-separated block");
    spoor_clunk(c);

    // --- the per-call clamp is a SHORT READ, not a truncation ---------------
    //
    // One call copies at most DEVPROC_ENVIRON_READ_MAX (8 KiB) because it runs
    // with IRQs off; the FILE is unbounded. The property that makes that safe is
    // that a consumer looping from the returned offset still gets everything --
    // so build a block past the clamp and prove the loop completes. (Off the
    // stack: 12 KiB of environment needs a real buffer.)
    for (int i = 0; i < 3; i++) {
        char nm[4] = { 'V', (char)('0' + i), '\0', '\0' };
        u64 id = env_create(tgt, nm, 2);
        TEST_ASSERT(id != 0, "create a big var");
        char chunk[512];
        for (size_t k = 0; k < sizeof(chunk); k++) chunk[k] = (char)('a' + i);
        for (int off = 0; off < 4096; off += (int)sizeof(chunk)) {
            TEST_EXPECT_EQ(env_write(tgt, id, off, chunk, (long)sizeof(chunk)),
                           (long)sizeof(chunk), "fill 4 KiB");
        }
    }
    // 2 small records + 3 * (2 name + 1 '=' + 4096 value + 1 NUL) = 12307 bytes.
    const long big_total = wlen + 3 * (2 + 1 + 4096 + 1);
    TEST_ASSERT(big_total > 8192L, "the block really does exceed the clamp");

    struct page *pg = alloc_pages(2, KP_ZERO);          // 16 KiB
    TEST_ASSERT(pg != NULL, "alloc a 16 KiB read buffer");
    char *big = (char *)pa_to_kva(page_to_pa(pg));

    struct Spoor *c2 = open_pidfile_for(tgt->pid, "environ", 0);
    TEST_ASSERT(c2 != NULL, "re-open environ");
    long first = devproc.read(c2, big, 16384L, 0);
    TEST_EXPECT_EQ(first, 8192L,
                   "V-4b-6: one call clamps at DEVPROC_ENVIRON_READ_MAX");

    long total = first;
    for (int guard = 0; guard < 8; guard++) {
        long k = devproc.read(c2, big + total, 16384L - total, total);
        if (k <= 0) break;
        total += k;
    }
    TEST_EXPECT_EQ(total, big_total,
                   "V-4b-6: looping past the clamp reads the WHOLE environment");
    // The first two records survived the big ones -- so the clamp shortened the
    // call, it did not drop content (the failure mode a format-and-slice render
    // would have had).
    same = true;
    for (long i = 0; same && i < wlen; i++) same = (big[i] == want[i]);
    TEST_ASSERT(same, "V-4b-6: the head of the block is unchanged by the clamp");
    spoor_clunk(c2);
    free_pages(pg, 2);

    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);
}

void test_devproc_read_cwd(void) {
    // (1) A Proc that never chdir'd holds the NULL dot sentinel, which renders
    //     "/": the never-empty property. It needs its own fresh Territory --
    //     kproc's dot is stamped by the boot, below.
    struct Proc *tgt = proc_alloc();
    TEST_ASSERT(tgt != NULL, "alloc cwd target");
    tgt->territory = territory_alloc();
    tgt->state = PROC_STATE_ALIVE;
    proc_test_link(tgt);

    char fresh[64];
    for (size_t i = 0; i < sizeof(fresh); i++) fresh[i] = (char)0xAA;
    long nf = -999;
    struct Spoor *cf = tgt->territory ? open_pidfile_for(tgt->pid, "cwd", 0) : NULL;
    if (cf) { nf = devproc.read(cf, fresh, (long)sizeof(fresh), 0); spoor_clunk(cf); }

    proc_test_unlink(tgt);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);                      // drops the Territory with the Proc

    // (2) kproc: joey_root_kproc_at_devramfs ran before the suite and put its
    //     dot where the initrd keeps its programs, so every boot child inherits
    //     /bin. Also pins the render of a non-sentinel dot.
    char kbuf[64];
    for (size_t i = 0; i < sizeof(kbuf); i++) kbuf[i] = (char)0xAA;
    long nk = -999;
    struct Spoor *ck = open_pidfile_for(0, "cwd", 0);
    if (ck) { nk = devproc.read(ck, kbuf, (long)sizeof(kbuf), 0); spoor_clunk(ck); }

    TEST_EXPECT_EQ(nf, 1L, "V-4b-1: an un-chdir'd Proc's cwd is \"/\" (1 byte)");
    TEST_ASSERT(nf == 1 && fresh[0] == '/',
                "V-4b-1: cwd renders \"/\" -- bare, no NUL, no newline");
    // Revert-probe anchor: appending a terminator makes n == 2 and fails above.
    TEST_ASSERT(fresh[1] == (char)0xAA, "V-4b-1: nothing written past the path");

    TEST_EXPECT_EQ(nk, 4L, "V-4b-1: kproc's cwd is the boot stamp \"/bin\" (4 bytes)");
    TEST_ASSERT(nk == 4 && kbuf[0] == '/' && kbuf[1] == 'b' && kbuf[2] == 'i' &&
                kbuf[3] == 'n', "V-4b-1: kproc's cwd renders \"/bin\"");
    TEST_ASSERT(kbuf[4] == (char)0xAA, "V-4b-1: nothing written past kproc's path");
}


// =============================================================================
// #133 -- the settled-park decision.
// =============================================================================
//
// A stale park must not read as stopped. wake_rendez_waiter sets
// state = THREAD_RUNNABLE and ready()s the thread but deliberately leaves
// rendez_blocked_on set, so between a resume's wakeup and the thread actually
// running, "registered" and "!on_cpu" BOTH hold on a thread that is leaving the
// park. Reading that as stopped is what made a `stop` following a `start` return
// early; the read that followed then landed after the thread had cleared
// rendez_blocked_on and before it re-parked, and answered EPERM.
//
// The RUNNABLE row is the regression. The rest of the table is here so a future
// edit cannot quietly drop one of the other two terms either.
void test_devproc_park_state_settled(void) {
    // Settled: in the park, not running, not woken.
    TEST_ASSERT(devproc_park_state_is_settled(true, false, THREAD_SLEEPING),
                "registered + off-cpu + SLEEPING is the settled park");

    // THE #133 ROW. Registered and off-cpu, but WOKEN -- a resume has already
    // readied it and it simply has not been dispatched yet. Reporting this as
    // stopped is the bug.
    TEST_ASSERT(!devproc_park_state_is_settled(true, false, THREAD_RUNNABLE),
                "a WOKEN-but-undispatched thread is NOT settled -- rendez_blocked_on "
                "outlives the wake, so the first two terms lie on their own");

    // The other two terms, unchanged by #133.
    TEST_ASSERT(!devproc_park_state_is_settled(false, false, THREAD_SLEEPING),
                "not registered on the debug rendez -> not parked at all");
    TEST_ASSERT(!devproc_park_state_is_settled(true, true, THREAD_SLEEPING),
                "on-cpu -> mid-switch; its ctx may be being written");

    // And a running thread is not settled however the other bits fall.
    TEST_ASSERT(!devproc_park_state_is_settled(true, false, THREAD_RUNNING),
                "RUNNING is not settled");
    TEST_ASSERT(!devproc_park_state_is_settled(false, true, THREAD_RUNNING),
                "nothing about a plainly-running thread is settled");

    // THE WIRING. The pure test above proves the DECISION; it is completely
    // blind to devproc_all_threads_parked passing the wrong arguments -- a call
    // site that hardcoded THREAD_SLEEPING would leave every row above green.
    // So drive the walk itself, over a stack Thread whose state is the only
    // thing that changes between the two assertions.
    //
    // Two zero-init stack Threads are not used here (a struct Thread `= {0}`
    // would emit a memset the freestanding kernel does not link, per the
    // focus-select test above); only the fields the predicate reads are set.
    struct Proc *pt = proc_alloc();
    TEST_ASSERT(pt != NULL, "alloc park-walk target");
    pt->state = PROC_STATE_ALIVE;
    struct Thread pth;
    pth.proc = pt;
    pth.next_in_proc = NULL;
    spin_lock_init(&pth.wait_lock);
    pth.rendez_blocked_on = &pth.debug_rendez;    // registered in the debug park
    pth.on_cpu = false;
    struct Thread *pt_saved = pt->threads;        // NULL for a fresh proc_alloc
    pt->threads = &pth;

    pth.state = THREAD_SLEEPING;
    TEST_ASSERT(devproc_all_threads_parked(pt),
                "the walk reports a SLEEPING registered peer as parked");
    pth.state = THREAD_RUNNABLE;
    TEST_ASSERT(!devproc_all_threads_parked(pt),
                "the walk reports a WOKEN registered peer as NOT parked -- this is "
                "the wiring, and the pure assertions above cannot see it");

    pt->threads = pt_saved;                       // unlink before free
    pt->state = PROC_STATE_ZOMBIE;
    proc_free(pt);
}

// =============================================================================
// IM-2: /proc/<pid>/imperium -- the legate scope as the kernel holds it, behind
// the owner-or-CAP_HOSTOWNER gate (the sched/environ posture).
// =============================================================================

void test_devproc_imperium_read_gated(void) {
    struct Proc *caller = proc_alloc();
    struct Proc *target = proc_alloc();
    TEST_ASSERT(caller && target, "proc_alloc caller + target");
    target->principal_id = 0xA11CEu;
    // A propagating scope worth reading: DAC|CHOWN flow, KILL held as a further
    // extra (the axe), a deadline. The block, then the scope_id RELEASE store.
    target->legate_session_id  = 77u;
    target->legate_caps        = CAP_DAC_OVERRIDE | CAP_CHOWN;
    target->legate_flags       = LEGATE_FLAG_PROPAGATING;
    target->legate_valid_until = 12345u;
    target->caps               = CAP_DAC_OVERRIDE | CAP_CHOWN | CAP_KILL;
    __atomic_store_n(&target->legate_scope_id, 94u, __ATOMIC_RELEASE);

    char buf[256];
    bool denied;
    size_t n;

    // Non-owner, no caps -> DENIED, zero bytes formatted (no partial leak).
    caller->principal_id = 0xB0Bu;
    caller->caps         = 0;
    denied = false;
    n = devproc_imperium_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(denied && n == 0, "non-owner imperium read denied, no bytes");

    // CAP_DAC_OVERRIDE is NOT an axis: an info file is owner-or-hostowner.
    caller->caps = CAP_DAC_OVERRIDE;
    denied = false;
    n = devproc_imperium_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(denied && n == 0, "CAP_DAC_OVERRIDE is not a read axis");

    // Owner -> the line, every field as held.
    caller->principal_id = 0xA11CEu;
    caller->caps         = 0;
    denied = true;
    n = devproc_imperium_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(!denied && n > 0, "owner imperium read allowed");
    TEST_ASSERT(contains(buf, n, "scope 94 session 77 propagating 1 rods 2 axe 1 caps 0x380 until 12345\n"),
                "imperium line: scope/session/propagating/rods/axe/caps/until");

    // CAP_HOSTOWNER (non-owner) -> allowed.
    caller->principal_id = 0xB0Bu;
    caller->caps         = CAP_HOSTOWNER;
    denied = true;
    n = devproc_imperium_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(!denied && n > 0, "CAP_HOSTOWNER imperium read allowed");

    // A plain (non-propagating) member with nothing flowing: rods 0, axe 0.
    target->legate_flags = 0u;
    target->legate_caps  = 0;
    target->caps         = CAP_JIT;
    n = devproc_imperium_read_gated(caller, target, buf, sizeof(buf), &denied);
    TEST_ASSERT(!denied && contains(buf, n, "propagating 0 rods 0 axe 0 caps 0x800 until 12345"),
                "plain member line: nothing flows, JIT held shows in caps only");

    caller->state = PROC_STATE_ZOMBIE;
    target->state = PROC_STATE_ZOMBIE;
    proc_free(caller);
    proc_free(target);
}

void test_devproc_read_imperium_format(void) {
    struct Spoor *root   = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *imp    = walk_one(piddir, "imperium");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(imp != NULL, "walk to /proc/0/imperium OK");
    TEST_ASSERT(devproc.open(imp, 0) != NULL, "open imperium");

    char buf[256];
    long got = devproc.read(imp, buf, (long)sizeof(buf), 0);
    TEST_ASSERT(got > 0, "imperium read positive (owner-gated allow for kproc-self)");
    // kproc: no scope, CAP_ALL holds no elevation-only bit.
    TEST_ASSERT(contains(buf, (size_t)got,
                         "scope 0 session 0 propagating 0 rods 0 axe 0 caps 0x0 until 0\n"),
                "kproc imperium line is the not-a-legate line");

    spoor_clunk(imp);
}

// =============================================================================
// The image join (H3): a guard on a Proc's IMAGE is evaluated over every Proc
// that MAPS that image, not over the named Proc alone.
// =============================================================================
//
// THE DEFECT, and why the ordinary cover test above could not see it: the
// capability-cover rule, both seal bits and the debug taint are per-PROC facts,
// while the image they guard lives in the AddrSpace, which rfork(RFPROC|RFMEM)
// SHARES. An elevated parent's vfork child is born with the elevation-only caps
// carved off (I-2), so it is a LOWER-authority Proc holding the SAME bytes --
// and a peer that merely matches the child covers it, attaches, and writes the
// parent's live image. musl's posix_spawn is exactly this shape, on every call.
//
// The fixture is that shape: E holds CAP_KILL, C shares E's address space with
// no caps at all, and B is C's equal. RED before the fix -- B was admitted to C,
// because C's own caps word is empty.
void test_devproc_image_cover_join(void) {
    struct Proc *e    = proc_alloc();
    struct Proc *c    = e ? proc_alloc_in(e->as, e->page_budget) : NULL;
    struct Proc *solo = proc_alloc();
    struct Proc *b    = proc_alloc();
    bool allocated = e && c && solo && b;
    bool shared_premise = false, solo_premise = false;
    bool peer_into_shared = true, covering_peer = false, peer_into_solo = false;
    bool peer_into_e = true;

    if (allocated) {
        e->principal_id = c->principal_id = solo->principal_id = 0xA11CEu;
        b->principal_id = 0xA11CEu;                       // the OWNER axis throughout
        e->state = c->state = solo->state = PROC_STATE_ALIVE;
        e->caps    = CAP_KILL;                            // the elevated parent
        c->caps    = 0;                                   // the carve stripped the child
        solo->caps = 0;                                   // same caps, no sharing
        proc_test_link(e);
        proc_test_link(c);
        proc_test_link(solo);

        // The premise, asserted rather than assumed: a fixture that failed to
        // share would satisfy every refusal below for the wrong reason.
        shared_premise = (e->as != NULL && c->as == e->as &&
                          addrspace_ref_count(e->as) == 2);
        solo_premise   = (solo->as != NULL && solo->as != e->as &&
                          addrspace_ref_count(solo->as) == 1);

        b->caps = 0;
        peer_into_shared = devproc_debug_authorized(b, c);   // THE REGRESSION
        peer_into_e      = devproc_debug_authorized(b, e);   // already refused pre-fix
        peer_into_solo   = devproc_debug_authorized(b, solo);
        b->caps = CAP_KILL;
        covering_peer    = devproc_debug_authorized(b, c);

        proc_test_unlink(solo);
        proc_test_unlink(c);
        proc_test_unlink(e);
    }
    if (c)    { c->state    = PROC_STATE_ZOMBIE; proc_free(c); }
    if (e)    { e->state    = PROC_STATE_ZOMBIE; proc_free(e); }
    if (solo) { solo->state = PROC_STATE_ZOMBIE; proc_free(solo); }
    if (b)    { b->state    = PROC_STATE_ZOMBIE; proc_free(b); }

    TEST_ASSERT(allocated, "alloc the sharing pair, the solo control and the peer");
    TEST_ASSERT(shared_premise, "premise: E and C really share one address space (ref 2)");
    TEST_ASSERT(solo_premise, "premise: the solo control shares with nobody (ref 1)");
    TEST_ASSERT(!peer_into_shared,
                "the join: a peer that does not cover the SHARER is refused the shared image");
    TEST_ASSERT(!peer_into_e,
                "control: the same peer was already refused E itself, on E's own caps");
    // The two controls that make the refusal mean something: the instrument
    // admits when the caller DOES cover the join, and an identical Proc that
    // merely shares with nobody is still debuggable. Without the second, a
    // blanket "refuse everything" would pass this test.
    TEST_ASSERT(covering_peer,
                "control: a peer that covers the whole image IS admitted");
    TEST_ASSERT(peer_into_solo,
                "control: an unshared Proc with the same empty caps is still debuggable");
}

// The seals over the image. Two mechanisms, tested apart because each must be
// able to fail alone:
//   - the JOIN reads another mapper's bit at the gate (this test);
//   - the STAMP puts the bit on every mapper when a Proc seals (the next).
// Here the sharer's bit is written DIRECTLY rather than through proc_seal,
// precisely so the stamp cannot supply the answer: with the stamp doing the work
// the target would carry its own bit and the old per-Proc read would pass too.
void test_devproc_image_seal_join(void) {
    struct Proc *e = proc_alloc();
    struct Proc *c = e ? proc_alloc_in(e->as, e->page_budget) : NULL;
    struct Proc *b = proc_alloc();
    bool allocated = e && c && b;
    bool premise = false, before_seal = false, after_notrace = true;
    long maps_before = -2, maps_after = -2, environ_after = -2;
    char buf[512];

    if (allocated) {
        struct Thread *th = current_thread();
        e->principal_id = c->principal_id = th && th->proc ? th->proc->principal_id
                                                           : 0xA11CEu;
        b->principal_id = e->principal_id;
        e->state = c->state = PROC_STATE_ALIVE;
        e->caps = c->caps = 0;
        b->caps = 0;
        proc_test_link(e);
        proc_test_link(c);
        premise = (e->as != NULL && c->as == e->as &&
                   addrspace_ref_count(e->as) == 2 &&
                   (c->proc_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE)) == 0);

        before_seal = devproc_debug_authorized(b, c);

        // NODUMP on the OTHER mapper: reading C's maps hands out E's sealed
        // layout, so the read must refuse. environ is the one-variable control --
        // per-Proc state that no sharer holds a copy of, so it must NOT be
        // sealed by a sibling's bit.
        struct Spoor *m1 = open_pidfile_for(c->pid, "maps", 0);
        maps_before = m1 ? devproc.read(m1, buf, (long)sizeof(buf), 0) : -2;
        __atomic_fetch_or(&e->proc_flags, PROC_FLAG_NODUMP, __ATOMIC_RELAXED);
        maps_after = m1 ? devproc.read(m1, buf, (long)sizeof(buf), 0) : -2;
        if (m1) spoor_clunk(m1);
        struct Spoor *en = open_pidfile_for(c->pid, "environ", 0);
        environ_after = en ? devproc.read(en, buf, (long)sizeof(buf), 0) : -2;
        if (en) spoor_clunk(en);

        // NOTRACE on the other mapper refuses CONTROL of this one.
        __atomic_fetch_or(&e->proc_flags, PROC_FLAG_NOTRACE, __ATOMIC_RELAXED);
        after_notrace = devproc_debug_authorized(b, c);

        proc_test_unlink(c);
        proc_test_unlink(e);
    }
    if (c) { c->state = PROC_STATE_ZOMBIE; proc_free(c); }
    if (e) { e->state = PROC_STATE_ZOMBIE; proc_free(e); }
    if (b) { b->state = PROC_STATE_ZOMBIE; proc_free(b); }

    TEST_ASSERT(allocated, "alloc the sharing pair + the peer");
    TEST_ASSERT(premise, "premise: the pair shares one space and C carries neither seal bit");
    TEST_ASSERT(before_seal, "control: unsealed, the peer is admitted to C");
    TEST_ASSERT(maps_before >= 0, "control: C's maps reads while the image is unsealed");
    TEST_EXPECT_EQ(maps_after, (long)-1,
                   "the join: NODUMP on a SHARER refuses C's maps (it is E's layout)");
    TEST_ASSERT(environ_after >= 0,
                "control: environ is per-Proc, so a sharer's NODUMP does not seal it");
    TEST_ASSERT(!after_notrace,
                "the join: NOTRACE on a SHARER refuses control of C");
}

// The stamp: sealing a Proc seals every Proc that maps its image. Through
// proc_seal, the production writer, so a regression in the real path is what
// this catches. RED before the fix -- the bit landed on the sealer alone, and
// its own vfork child stayed an unsealed door to the same bytes.
void test_proc_seal_stamps_the_image(void) {
    struct Proc *e    = proc_alloc();
    struct Proc *c    = e ? proc_alloc_in(e->as, e->page_budget) : NULL;
    struct Proc *solo = proc_alloc();
    bool allocated = e && c && solo;
    bool premise = false;
    u32 c_flags = 0, e_flags = 0, solo_flags = 0;

    if (allocated) {
        e->state = c->state = solo->state = PROC_STATE_ALIVE;
        proc_test_link(e);
        proc_test_link(c);
        proc_test_link(solo);
        premise = (e->as != NULL && c->as == e->as &&
                   addrspace_ref_count(e->as) == 2 &&
                   (c->proc_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE)) == 0);

        proc_seal(e, PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE);

        e_flags    = __atomic_load_n(&e->proc_flags, __ATOMIC_ACQUIRE);
        c_flags    = __atomic_load_n(&c->proc_flags, __ATOMIC_ACQUIRE);
        solo_flags = __atomic_load_n(&solo->proc_flags, __ATOMIC_ACQUIRE);

        proc_test_unlink(solo);
        proc_test_unlink(c);
        proc_test_unlink(e);
    }
    if (c)    { c->state    = PROC_STATE_ZOMBIE; proc_free(c); }
    if (e)    { e->state    = PROC_STATE_ZOMBIE; proc_free(e); }
    if (solo) { solo->state = PROC_STATE_ZOMBIE; proc_free(solo); }

    TEST_ASSERT(allocated, "alloc the sharing pair + the unrelated control");
    TEST_ASSERT(premise, "premise: the pair shares one space and C starts unsealed");
    TEST_ASSERT((e_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE))
                        == (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE),
                "control: the sealer itself carries both bits");
    TEST_ASSERT((c_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE))
                        == (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE),
                "the stamp: sealing E seals C, which maps the same image");
    TEST_ASSERT((solo_flags & (PROC_FLAG_NODUMP | PROC_FLAG_NOTRACE)) == 0,
                "control: an unrelated Proc is NOT swept up by the stamp");
}

// Audit F3's regression. A ZOMBIE mapper must NOT refuse its parent's elevation.
// The address-space reference outlives the Proc until the reap, so counting
// references alone is a FALSE POSITIVE, not a hardening: a non-ALIVE Proc can be
// neither attached to nor stopped, so it is no door to the image. A vfork child
// that _exits instead of exec'ing puts its parent in exactly that state, and a
// parent that never waits would never elevate again.
// The ALIVE pair is the control ONE VARIABLE away -- same two mappers, same
// reference count of 2, differing only in the sharer's state -- so a build that
// simply stopped refusing anything cannot pass both legs.
void test_proc_elevation_ignores_a_zombie_sharer(void) {
    struct Proc *e    = proc_alloc();
    struct Proc *dead = e ? proc_alloc_in(e->as, e->page_budget) : NULL;
    struct Proc *live = proc_alloc();
    struct Proc *sib  = live ? proc_alloc_in(live->as, live->page_budget) : NULL;
    bool allocated = e && dead && live && sib;
    bool premise = false;
    int zombie_rc = 1, alive_rc = 0;

    if (allocated) {
        struct Thread *th = current_thread();
        u32 me = th && th->proc ? th->proc->principal_id : 0xA11CEu;
        e->principal_id = dead->principal_id = me;
        live->principal_id = sib->principal_id = me;
        e->caps = dead->caps = live->caps = sib->caps = 0;
        e->state = live->state = sib->state = PROC_STATE_ALIVE;
        // LINKED first: the subtraction counts only the zombies the traversal
        // SAW, so an unlinked zombie would fall in the difference and keep
        // `shared` true -- this test would then pass for the wrong reason.
        proc_test_link(e);
        proc_test_link(dead);
        proc_test_link(live);
        proc_test_link(sib);
        dead->state = PROC_STATE_ZOMBIE;

        premise = (e->as != NULL && dead->as == e->as &&
                   addrspace_ref_count(e->as) == 2 &&
                   live->as != NULL && sib->as == live->as &&
                   addrspace_ref_count(live->as) == 2 &&
                   dead->state == PROC_STATE_ZOMBIE &&
                   sib->state == PROC_STATE_ALIVE &&
                   (e->proc_flags & PROC_FLAG_DEBUG_TAINTED) == 0 &&
                   (live->proc_flags & PROC_FLAG_DEBUG_TAINTED) == 0);

        zombie_rc = proc_become_legate(e, CAP_KILL, 1u, 0u, 0u);
        alive_rc  = proc_become_legate(live, CAP_KILL, 1u, 0u, 0u);

        proc_test_unlink(sib);
        proc_test_unlink(live);
        proc_test_unlink(dead);
        proc_test_unlink(e);
    }
    if (sib)  { sib->state  = PROC_STATE_ZOMBIE; proc_free(sib); }
    if (live) { live->state = PROC_STATE_ZOMBIE; proc_free(live); }
    if (dead) { proc_free(dead); }                 // already a ZOMBIE above
    if (e)    { e->state    = PROC_STATE_ZOMBIE; proc_free(e); }

    TEST_ASSERT(allocated, "alloc the zombie-sharer and live-sharer pairs");
    TEST_ASSERT(premise,
                "premise: both pairs really share one image at ref 2, one sharer "
                "dead and one alive, neither parent tainted");
    TEST_EXPECT_EQ(zombie_rc, 0,
        "a ZOMBIE sharer does not refuse its parent's elevation");
    TEST_ASSERT(alive_rc != 0, "control: an ALIVE sharer still refuses it");
}

// The debug taint (C): a Proc whose image has been under debug CONTROL never
// gains authority again -- Linux's LSM_UNSAFE_PTRACE half, which the cover rule
// alone does not supply. Driven through the REAL attach write, so the production
// stamp site is what is under test, and refused at the REAL legate stamp.
//
// The attack it closes: a same-principal peer of EQUAL authority is admitted by
// cover, attaches to a Proc before that Proc redeems a clearance, writes its
// stack, detaches -- and the redeem then returns through the peer's address
// holding the elevation. The cover rule is point-in-time and sees none of it.
void test_proc_debug_taint_refuses_elevation(void) {
    struct Proc *clean   = proc_alloc();
    struct Proc *debugged = proc_alloc();
    struct Proc *e       = proc_alloc();
    struct Proc *shared  = e ? proc_alloc_in(e->as, e->page_budget) : NULL;
    bool allocated = clean && debugged && e && shared;
    bool premise = false;
    long attached = -1, detached = -1;
    int  clean_rc = 1, debugged_rc = 0, shared_rc = 0;
    u32  tainted_flags = 0;
    caps_t debugged_caps = CAP_ALL;

    if (allocated) {
        struct Thread *th = current_thread();
        u32 me = th && th->proc ? th->proc->principal_id : 0xA11CEu;
        clean->principal_id = debugged->principal_id = me;
        e->principal_id = shared->principal_id = me;
        clean->state = debugged->state = PROC_STATE_ALIVE;
        e->state = shared->state = PROC_STATE_ALIVE;
        clean->caps = debugged->caps = e->caps = shared->caps = 0;
        proc_test_link(clean);
        proc_test_link(debugged);
        proc_test_link(e);
        proc_test_link(shared);
        // BOTH fixtures' premises, not just the first: without the second, the
        // sharing assertion below is equally satisfied by a spuriously-tainted
        // `shared` -- two causes, one reading.
        premise = ((debugged->proc_flags & PROC_FLAG_DEBUG_TAINTED) == 0 &&
                   (shared->proc_flags & PROC_FLAG_DEBUG_TAINTED) == 0 &&
                   (e->proc_flags & PROC_FLAG_DEBUG_TAINTED) == 0 &&
                   e->as != NULL && shared->as == e->as &&
                   addrspace_ref_count(e->as) == 2);

        // The control FIRST: an untouched Proc elevates normally, so the
        // refusals below are the taint and the sharing rather than a legate
        // stamp that simply stopped working.
        clean_rc = proc_become_legate(clean, CAP_KILL, 1u, 0u, 0u);

        struct Spoor *ctl = open_ctl_for_pid(debugged->pid);
        if (ctl) {
            attached = devproc.write(ctl, "attach", 6, 0);
            detached = devproc.write(ctl, "detach", 6, 0);
            spoor_clunk(ctl);
        }
        tainted_flags = __atomic_load_n(&debugged->proc_flags, __ATOMIC_ACQUIRE);
        debugged_rc   = proc_become_legate(debugged, CAP_KILL, 1u, 0u, 0u);
        debugged_caps = __atomic_load_n(&debugged->caps, __ATOMIC_ACQUIRE);

        // Never debugged, but its image is still shared: a peer holding the
        // other door must not be handed this Proc's new authority.
        shared_rc = proc_become_legate(shared, CAP_KILL, 1u, 0u, 0u);

        proc_test_unlink(shared);
        proc_test_unlink(e);
        proc_test_unlink(debugged);
        proc_test_unlink(clean);
    }
    if (shared)   { shared->state   = PROC_STATE_ZOMBIE; proc_free(shared); }
    if (e)        { e->state        = PROC_STATE_ZOMBIE; proc_free(e); }
    if (debugged) { debugged->state = PROC_STATE_ZOMBIE; proc_free(debugged); }
    if (clean)    { clean->state    = PROC_STATE_ZOMBIE; proc_free(clean); }

    TEST_ASSERT(allocated, "alloc the clean, debugged and sharing fixtures");
    TEST_ASSERT(premise, "premise: the target starts untainted and the pair really shares");
    TEST_EXPECT_EQ(clean_rc, 0, "control: an untouched Proc still becomes a legate");
    TEST_EXPECT_EQ(attached, (long)6, "premise: the real attach write succeeded");
    TEST_EXPECT_EQ(detached, (long)6, "premise: the real detach write succeeded");
    TEST_ASSERT(tainted_flags & PROC_FLAG_DEBUG_TAINTED,
                "the attach stamped the taint on the target");
    TEST_ASSERT(debugged_rc != 0,
                "the taint: a Proc that has been attached does not become a legate");
    TEST_EXPECT_EQ((long)debugged_caps, (long)0,
                   "the refused redeem conferred no caps at all");
    TEST_ASSERT(shared_rc != 0,
                "sharing: a Proc whose image another Proc maps does not become a legate");
}

// The taint crosses FORK -- the one arm of the taint the image join can never
// cover, and therefore the one that has to be inherited rather than derived.
//
// Once a child execs it has a fresh address space and shares nothing, so the
// join has no mapper left to read and only a copied bit still carries the
// history. Without it the attack is trivial: a peer drives the shell it has
// tainted into forking a CLEAN child, and that child elevates instead.
//
// Driven through the REAL rfork, on the real publication path, because the
// inherit lives inside `rfork_internal`'s publication lock hold and a helper
// would pin nothing. The fixture is kproc -- the Proc a kernel test runs on --
// so the bit is RESTORED before any assertion runs: TEST_ASSERT returns on
// failure, and a taint left on kproc would be inherited by every later test's
// children and surface as an unrelated failure somewhere else (the reap-before-
// asserting lesson from test_addrspace's rfork leg, applied to a fixture that
// is shared rather than private).
static volatile u32 g_fork_inherit_child_flags;
static void fork_inherit_thunk(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    g_fork_inherit_child_flags =
        (t && t->proc) ? __atomic_load_n(&t->proc->proc_flags, __ATOMIC_ACQUIRE) : 0u;
    exits("ok");
}

void test_proc_debug_taint_crosses_fork(void) {
    struct Proc *me = kproc();
    TEST_ASSERT(me != NULL, "kproc()");
    bool premise = (__atomic_load_n(&me->proc_flags, __ATOMIC_ACQUIRE)
                    & PROC_FLAG_DEBUG_TAINTED) == 0;

    // Leg 1: a tainted parent's child carries the bit.
    __atomic_fetch_or(&me->proc_flags, PROC_FLAG_DEBUG_TAINTED, __ATOMIC_RELAXED);
    g_fork_inherit_child_flags = 0xFFFFFFFFu;            // a value neither arm produces
    int pid = rfork(RFPROC, fork_inherit_thunk, NULL);
    int st = -1;
    int reaped = (pid > 0) ? wait_pid_for(pid, 0, &st) : -1;
    u32 tainted_child = g_fork_inherit_child_flags;

    // RESTORE before the control leg, and before every assertion.
    __atomic_and_fetch(&me->proc_flags, ~PROC_FLAG_DEBUG_TAINTED, __ATOMIC_RELAXED);

    // Leg 2, the control one variable away: with the parent clean again, the
    // child must come out clean. Without it, a child born tainted for any other
    // reason would satisfy leg 1.
    g_fork_inherit_child_flags = 0xFFFFFFFFu;
    int cpid = rfork(RFPROC, fork_inherit_thunk, NULL);
    int cst = -1;
    int creaped = (cpid > 0) ? wait_pid_for(cpid, 0, &cst) : -1;
    u32 clean_child = g_fork_inherit_child_flags;

    bool restored = (__atomic_load_n(&me->proc_flags, __ATOMIC_ACQUIRE)
                     & PROC_FLAG_DEBUG_TAINTED) == 0;

    TEST_ASSERT(premise, "premise: kproc starts untainted");
    TEST_ASSERT(pid > 0 && reaped == pid, "the tainted-parent fork spawned and reaped");
    TEST_ASSERT(cpid > 0 && creaped == cpid, "the clean-parent fork spawned and reaped");
    TEST_ASSERT(tainted_child != 0xFFFFFFFFu, "the child recorded its own flags");
    TEST_ASSERT(tainted_child & PROC_FLAG_DEBUG_TAINTED,
                "the taint crosses fork: a tainted parent's child is born tainted");
    TEST_ASSERT(clean_child != 0xFFFFFFFFu, "the control child recorded its own flags");
    TEST_ASSERT((clean_child & PROC_FLAG_DEBUG_TAINTED) == 0,
                "control: a clean parent's child is born clean");
    TEST_ASSERT(restored, "the fixture was restored -- kproc is untainted again");
}
