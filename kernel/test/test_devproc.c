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

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
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
// 8a-1b: the I-39 debug gate + the attach/detach/close slot lifecycle.
void test_devproc_debug_authorized_predicate(void);
void test_devproc_debug_attach_detach_lifecycle(void);
void test_devproc_debug_stop_start_resume(void);
void test_devproc_debug_mem(void);
void test_devproc_debug_regs(void);
void test_devproc_debug_kregs_kstack_wait(void);
void test_devproc_debug_step_cancel_on_stop(void);

// A-4b + 8a-1b impl hooks (non-static in kernel/devproc.c) + Proc test helpers
// (non-static in kernel/proc.c; the test_proc.c / test_devsrv_conn.c pattern).
bool devproc_kill_authorized(const struct Proc *caller, const struct Proc *target);
bool devproc_debug_authorized(const struct Proc *caller, const struct Proc *target);
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);

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

    char buf[256];
    long got = devproc.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "read returns positive byte count");

    TEST_ASSERT(contains(buf, (size_t)got, "pid:"),     "status contains 'pid:'");
    TEST_ASSERT(contains(buf, (size_t)got, "0"),        "status contains kproc pid '0'");
    TEST_ASSERT(contains(buf, (size_t)got, "state:"),   "status contains 'state:'");
    TEST_ASSERT(contains(buf, (size_t)got, "ALIVE"),    "kproc state is ALIVE");
    TEST_ASSERT(contains(buf, (size_t)got, "threads:"), "status contains 'threads:'");

    spoor_clunk(c);
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

// stat_native reports the target Proc as the per-pid object's owner, with the
// Plan 9 /proc mode convention (ctl 0600, info files 0444); the dev apex -> -1.
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
    TEST_EXPECT_EQ(st.mode, (u32)0600u,       "ctl mode = 0600 (owner-private)");
    TEST_EXPECT_EQ(st.qid_type, QTFILE,       "ctl is QTFILE");
    spoor_unref(ctl);

    struct Spoor *status = walk_one(piddir, "status");
    TEST_ASSERT(status != NULL, "walk to status");
    TEST_EXPECT_EQ(devproc.stat_native(status, &st), 0, "stat_native(status) ok");
    TEST_EXPECT_EQ(st.mode, (u32)0444u, "status mode = 0444 (world-readable)");
    spoor_unref(status);
    spoor_unref(piddir);

    struct Spoor *root2 = devproc.attach("");
    TEST_EXPECT_EQ(devproc.stat_native(root2, &st), -1,
                   "stat_native(dev apex) = -1 (no per-Proc owner)");
    spoor_unref(root2);

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
    target->proc_flags |= PROC_FLAG_NOTRACE;
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
    TEST_ASSERT(tgt->pgtable_root != 0, "target has a pgtable_root");
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
    TEST_EXPECT_EQ(mmu_install_user_pte(tgt->pgtable_root, 0, RW_VA, rw_pa, VMA_PROT_RW,   false), 0, "map RW page");
    TEST_EXPECT_EQ(mmu_install_user_pte(tgt->pgtable_root, 0, RO_VA, ro_pa, VMA_PROT_READ, false), 0, "map RO page");

    // --- Layer 1: the raw cross-Proc resolver ---
    u8 buf[64], wbuf[64];
    long got = mmu_cross_proc_read(tgt->pgtable_root, RW_VA, buf, 64);
    TEST_EXPECT_EQ(got, 64L, "cross_proc_read reads the RW page span");
    bool match = true; for (int i = 0; i < 64; i++) if (buf[i] != (u8)(0xA0 + i)) match = false;
    TEST_ASSERT(match, "cross_proc_read returns the RW page bytes");

    for (int i = 0; i < 64; i++) wbuf[i] = (u8)(0x11 + i);
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->pgtable_root, RW_VA, wbuf, 64), 64L, "cross_proc_write writes the RW page");
    match = true; for (int i = 0; i < 64; i++) if (rw_kva[i] != (u8)(0x11 + i)) match = false;
    TEST_ASSERT(match, "cross_proc_write landed the bytes in the target page");

    // RO leaf: read OK, write REFUSED (W^X / Image cache) + the page untouched.
    TEST_EXPECT_EQ(mmu_cross_proc_read(tgt->pgtable_root, RO_VA, buf, 64), 64L, "cross_proc_read reads an RO page");
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->pgtable_root, RO_VA, wbuf, 64), 0L, "cross_proc_write REFUSES an RO leaf");
    TEST_ASSERT(ro_kva[0] == 0x50, "the RO page was NOT modified by the refused write");

    // A non-resident VA -> 0 (not resident; no fault-in).
    TEST_EXPECT_EQ(mmu_cross_proc_read(tgt->pgtable_root,  GAP_VA, buf,  64), 0L, "read of a hole returns 0");
    TEST_EXPECT_EQ(mmu_cross_proc_write(tgt->pgtable_root, GAP_VA, wbuf, 64), 0L, "write of a hole returns 0");

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
    TEST_EXPECT_EQ(devproc.write(mem, wbuf, 64, (s64)RW_VA), 64L, "mem write of a stopped target");
    TEST_ASSERT(rw_kva[0] == 0x77, "mem write landed in the target page");
    TEST_EXPECT_EQ(devproc.read(mem, buf, 64, (s64)GAP_VA), 0L, "mem read of a hole returns 0");

    // Non-owner (a target owned by a different principal; caller has no debug
    // cap) -> refused even while stopped (I-39).
    TEST_ASSERT(!(caller->caps & (CAP_HOSTOWNER | CAP_DEBUG)),
                "test caller lacks CAP_HOSTOWNER/CAP_DEBUG (the denied case is meaningful)");
    tgt->principal_id = (caller->principal_id == 0x0D0D0D0Du) ? 0x0E0E0E0Eu : 0x0D0D0D0Du;
    TEST_EXPECT_EQ(devproc.read(mem, buf, 64, (s64)RW_VA), (long)-1,
                   "mem read by a non-owner (no CAP_DEBUG) is refused (I-39)");
    spoor_clunk(mem);

    // Cleanup: free MY backing pages (proc_pgtable_destroy leaves leaf data pages
    // to the VMA layer), then the tree via proc_free.
    proc_test_unlink(tgt);
    free_pages(rw_pg, 0);
    free_pages(ro_pg, 0);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);
}

// 8a-1b-gamma-2: /proc/<pid>/regs + fpregs -- the saved EL0 register frames of a
// STOPPED target's head thread (I-39; DEBUG-FS 4.5). Drives the full devproc
// path on a SYNTHETIC parked thread with a real kstack buffer + a known
// trapframe + FP ctx. The headline check is the SPSR (pstate) privilege guard:
// a regs write applies x0..x30 + sp + pc but NEVER SPSR -- an arbitrary SPSR
// could eret the target to EL1.
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

    // --- regs read ---
    struct Spoor *regs = open_pidfile_for(tgt->pid, "regs", 2);   // ORDWR
    TEST_ASSERT(regs != NULL, "open /proc/<pid>/regs");
    struct t_user_regs ur;
    TEST_EXPECT_EQ(devproc.read(regs, &ur, (long)sizeof(ur), 0), (long)sizeof(ur), "regs read: full struct");
    bool m = true; for (int i = 0; i < 31; i++) if (ur.regs[i] != 0x1000ull + (u64)i) m = false;
    TEST_ASSERT(m, "regs read: x0..x30");
    TEST_EXPECT_EQ(ur.sp,     0xDEAD0000ull, "regs read: sp = SP_EL0");
    TEST_EXPECT_EQ(ur.pc,     0xCAFE0000ull, "regs read: pc = ELR_EL1");
    TEST_EXPECT_EQ(ur.pstate, 0x60000000ull, "regs read: pstate = SPSR_EL1");

    // --- regs write: x0..x30 + sp + pc applied; pstate (SPSR) IGNORED (guard) ---
    // Field-wise init (every field is set below), NOT `wr = ur`: the 272-byte
    // struct copy lowers to a memcpy call the freestanding kernel does not link
    // under the UBSan (low-opt) build.
    struct t_user_regs wr;
    for (int i = 0; i < 31; i++) wr.regs[i] = 0x2000ull + (u64)i;
    wr.sp     = 0xBEEF0000ull;
    wr.pc     = 0xF00D0000ull;
    wr.pstate = 0x00000005ull;   // an EL1h-mode SPSR (M[3:0]=0b0101) -- MUST be ignored
    TEST_EXPECT_EQ(devproc.write(regs, &wr, (long)sizeof(wr), 0), (long)sizeof(wr), "regs write");
    m = true; for (int i = 0; i < 31; i++) if (tf->regs[i] != 0x2000ull + (u64)i) m = false;
    TEST_ASSERT(m, "regs write applied x0..x30");
    TEST_EXPECT_EQ(tf->sp,  0xBEEF0000ull, "regs write applied sp (SP_EL0)");
    TEST_EXPECT_EQ(tf->elr, 0xF00D0000ull, "regs write applied pc (ELR_EL1)");
    TEST_EXPECT_EQ(tf->spsr, 0x60000000ull,
                   "regs write did NOT change SPSR (the EL1-mode pstate was ignored -- privilege guard)");
    spoor_clunk(regs);

    // --- fpregs read + write (all fields; no privilege bits) ---
    struct Spoor *fp = open_pidfile_for(tgt->pid, "fpregs", 2);
    TEST_ASSERT(fp != NULL, "open /proc/<pid>/fpregs");
    struct t_user_fpregs uf;
    TEST_EXPECT_EQ(devproc.read(fp, &uf, (long)sizeof(uf), 0), (long)sizeof(uf), "fpregs read: full struct");
    m = true; for (int i = 0; i < 512; i++) if (uf.vregs[i] != (u8)(0x30 + (i & 0x3f))) m = false;
    TEST_ASSERT(m, "fpregs read: V0..V31");
    TEST_EXPECT_EQ(uf.fpsr, 0xFEEDFACEu, "fpregs read: fpsr");
    TEST_EXPECT_EQ(uf.fpcr, 0x0BADF00Du, "fpregs read: fpcr");
    uf.fpcr = 0x11112222u;
    TEST_EXPECT_EQ(devproc.write(fp, &uf, (long)sizeof(uf), 0), (long)sizeof(uf), "fpregs write");
    TEST_EXPECT_EQ(th.ctx.fpcr, 0x11112222u, "fpregs write applied fpcr");
    spoor_clunk(fp);

    // --- not-stopped -> refused (stopped-only) ---
    tgt->debug_stop_req = 0;
    struct Spoor *regs2 = open_pidfile_for(tgt->pid, "regs", 2);
    TEST_EXPECT_EQ(devproc.read(regs2, &ur, (long)sizeof(ur), 0), (long)-1,
                   "regs of a NOT-stopped target is refused");
    spoor_clunk(regs2);

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

    // Cleanup BEFORE the HF1 asserts: unlink + drop the synthetic (stack-local)
    // thread BEFORE the frame dies, free the kstack, then the Proc. TEST_ASSERT
    // returns on failure -- asserting first would leave tgt linked with a
    // dangling tgt->threads (the kregs lesson).
    proc_test_unlink(tgt);
    tgt->threads = NULL;
    free_pages(kstk, THREAD_KSTACK_TOTAL_ORDER);
    tgt->state = PROC_STATE_ZOMBIE;
    proc_free(tgt);

    TEST_EXPECT_EQ(hf1_msg_rc, (long)-1,
                   "regs of a group-terminating target is refused (HF1)");
    TEST_EXPECT_EQ(hf1_exiting_rc, (long)-1,
                   "regs of an EXITING head thread is refused (HF1 backstop)");
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
    struct t_kernel_regs kr;
    for (size_t i = 0; i < sizeof(kr); i++) ((u8 *)&kr)[i] = 0;
    long kregs_rlen = -999, kregs_wlen = -999;
    struct Spoor *kregs = open_pidfile_for(tgt->pid, "kregs", 0);   // OREAD
    if (kregs) {
        kregs_rlen = devproc.read(kregs, &kr, (long)sizeof(kr), 0);
        kregs_wlen = devproc.write(kregs, &kr, (long)sizeof(kr), 0);   // RO -> -1
        spoor_clunk(kregs);
    }

    char sbuf[512]; for (size_t i = 0; i < sizeof(sbuf); i++) sbuf[i] = 0;
    long slen = -999;
    struct Spoor *ks = open_pidfile_for(tgt->pid, "kstack", 0);
    if (ks) { slen = devproc.read(ks, sbuf, (long)sizeof(sbuf), 0); spoor_clunk(ks); }

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
    // kregs: the kernel-side saved frame + TLS base.
    TEST_EXPECT_EQ(kregs_rlen, (long)sizeof(kr), "kregs read: full struct");
    bool m = true;
    for (int i = 0; i < 10; i++) if (kr.x[i] != 0xC200ull + (u64)i) m = false;
    TEST_ASSERT(m, "kregs read: x19..x28 (callee-saved)");
    TEST_EXPECT_EQ(kr.fp,        (u64)(uintptr_t)fp0, "kregs read: fp = ctx.fp (kstack-walk start)");
    TEST_EXPECT_EQ(kr.lr,        0x11110000ull,       "kregs read: lr = ctx.lr");
    TEST_EXPECT_EQ(kr.sp,        0xC096ull,           "kregs read: sp");
    TEST_EXPECT_EQ(kr.tpidr_el0, 0xC104ull,           "kregs read: tpidr_el0 (TLS base for Delve's g)");
    TEST_EXPECT_EQ(kregs_wlen,   (long)-1,            "kregs is RO (write refused)");

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
