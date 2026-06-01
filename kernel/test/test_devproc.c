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

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

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

// A-4b impl hooks (non-static in kernel/devproc.c) + Proc test helpers
// (non-static in kernel/proc.c; the test_proc.c / test_devsrv_conn.c pattern).
bool devproc_kill_authorized(const struct Proc *caller, const struct Proc *target);
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
