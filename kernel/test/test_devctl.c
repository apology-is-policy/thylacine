// /ctl Dev tests (P4-D).
//
// Covers registration, walks, per-leaf reads, write rejection.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // V-4b-5: struct t_stat + T_S_IF*
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_devctl_bestiary_smoke(void);
void test_devctl_attach_returns_dir(void);
void test_devctl_walk_to_each_leaf(void);
void test_devctl_walk_unknown_misses(void);
void test_devctl_read_procs_format(void);
void test_devctl_read_memory_format(void);
void test_devctl_read_devices_format(void);
void test_devctl_read_kernel_base_format(void);
void test_devctl_kernel_base_gated(void);
void test_devctl_read_sched_format(void);
void test_devctl_read_cpu_format(void);
void test_devctl_write_rejected(void);
void test_devctl_read_dir_returns_neg1(void);
void test_devctl_stat_native_shapes(void);

// =============================================================================
// Helpers.
// =============================================================================

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

static struct Spoor *walk_one(struct Spoor *c, const char *name) {
    const char *names[1] = { name };
    struct Walkqid *wq = devctl.walk(c, NULL, names, 1);
    if (!wq) return NULL;
    if (wq->nqid != 1) {
        spoor_unref(wq->spoor);
        walkqid_free(wq);
        return NULL;
    }
    struct Spoor *r = wq->spoor;
    walkqid_free(wq);
    return r;
}

// Open /ctl/<name>; caller spoor_clunk's the result.
static struct Spoor *open_ctl_leaf(const char *name) {
    struct Spoor *root = devctl.attach("");
    if (!root) return NULL;
    struct Spoor *leaf = walk_one(root, name);
    spoor_unref(root);
    if (!leaf) return NULL;
    if (!devctl.open(leaf, 0)) {
        spoor_unref(leaf);
        return NULL;
    }
    return leaf;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devctl_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('C'),       &devctl, "lookup 'C' = devctl");
    TEST_EXPECT_EQ(dev_lookup_by_name("ctl"),   &devctl, "lookup 'ctl' = devctl");
    TEST_EXPECT_EQ(devctl.dc, 'C',                       "devctl.dc = 'C'");
}

void test_devctl_attach_returns_dir(void) {
    struct Spoor *c = devctl.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devctl_walk_to_each_leaf(void) {
    static const char *leaf_names[] = {
        "procs", "memory", "devices", "kernel-base", "sched", "cpu",
    };
    for (size_t i = 0; i < sizeof(leaf_names) / sizeof(leaf_names[0]); i++) {
        struct Spoor *root = devctl.attach("");
        struct Spoor *leaf = walk_one(root, leaf_names[i]);
        spoor_unref(root);
        TEST_ASSERT(leaf != NULL, "walk to leaf succeeds");
        TEST_EXPECT_EQ(leaf->qid.type, QTFILE, "leaf is QTFILE");
        TEST_ASSERT(leaf->qid.path != 0, "leaf path != root");
        spoor_unref(leaf);
    }
}

void test_devctl_walk_unknown_misses(void) {
    struct Spoor *root = devctl.attach("");
    const char *names[1] = { "does-not-exist" };
    struct Walkqid *wq = devctl.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk to unknown leaf misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

void test_devctl_read_procs_format(void) {
    struct Spoor *c = open_ctl_leaf("procs");
    TEST_ASSERT(c != NULL, "open /ctl/procs");

    char buf[512];
    long got = devctl.read(c, buf, 512, 0);
    TEST_ASSERT(got > 0, "procs read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "PID"),     "header has PID column");
    TEST_ASSERT(contains(buf, (size_t)got, "PPID"),    "prowl-4: header has the PPID (tree) column");
    TEST_ASSERT(contains(buf, (size_t)got, "STATE"),   "header has STATE");
    TEST_ASSERT(contains(buf, (size_t)got, "ALIVE"),   "kproc shows ALIVE");

    spoor_clunk(c);
}

// prowl-3b: /ctl/cpu -- the per-CPU meter denominator (cpus + per-CPU idle_ns +
// capacity). All-visible like the other coarse /ctl leaves.
void test_devctl_read_cpu_format(void) {
    struct Spoor *c = open_ctl_leaf("cpu");
    TEST_ASSERT(c != NULL, "open /ctl/cpu");

    char buf[512];
    long got = devctl.read(c, buf, 512, 0);
    TEST_ASSERT(got > 0, "cpu read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "cpus:"),    "has the cpus: count");
    TEST_ASSERT(contains(buf, (size_t)got, "idle_ns"),  "has the idle_ns column");
    TEST_ASSERT(contains(buf, (size_t)got, "capacity"), "has the capacity column");

    spoor_clunk(c);
}

void test_devctl_read_memory_format(void) {
    struct Spoor *c = open_ctl_leaf("memory");
    TEST_ASSERT(c != NULL, "open /ctl/memory");

    char buf[256];
    long got = devctl.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "memory read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "total:"),    "has total:");
    TEST_ASSERT(contains(buf, (size_t)got, "free:"),     "has free:");
    TEST_ASSERT(contains(buf, (size_t)got, "reserved:"), "has reserved:");
    TEST_ASSERT(contains(buf, (size_t)got, "pages"),     "uses page units");

    spoor_clunk(c);
}

void test_devctl_read_devices_format(void) {
    struct Spoor *c = open_ctl_leaf("devices");
    TEST_ASSERT(c != NULL, "open /ctl/devices");

    char buf[256];
    long got = devctl.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "devices read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "DC"),     "header has DC column");
    TEST_ASSERT(contains(buf, (size_t)got, "NAME"),   "header has NAME column");
    TEST_ASSERT(contains(buf, (size_t)got, "none"),   "lists devnone");
    TEST_ASSERT(contains(buf, (size_t)got, "cons"),   "lists devcons");
    TEST_ASSERT(contains(buf, (size_t)got, "ctl"),    "lists devctl itself");
    TEST_ASSERT(contains(buf, (size_t)got, "proc"),   "lists devproc");

    spoor_clunk(c);
}

void test_devctl_read_kernel_base_format(void) {
    // #57a F1: /ctl/kernel-base is CAP_HOSTOWNER-gated (the KASLR slide, an
    // I-16 secret; CAP_HOSTOWNER is elevation-only -- not even kproc holds it
    // by default). Temporarily elevate the in-kernel test thread to exercise
    // the format through the REAL gated read path (an elevated admin reading
    // the slide). Restore BEFORE the content asserts so a failing assert can
    // never leave kproc elevated. The deny path is test_devctl_kernel_base_gated.
    struct Thread *t = current_thread();
    u64 saved = __atomic_load_n(&t->proc->caps, __ATOMIC_ACQUIRE);
    __atomic_store_n(&t->proc->caps, saved | CAP_HOSTOWNER, __ATOMIC_RELEASE);

    struct Spoor *c = open_ctl_leaf("kernel-base");
    char buf[256];
    long got = c ? devctl.read(c, buf, 256, 0) : -1;

    __atomic_store_n(&t->proc->caps, saved, __ATOMIC_RELEASE);  // restore first

    TEST_ASSERT(c != NULL, "open /ctl/kernel-base");
    TEST_ASSERT(got > 0, "kernel-base read positive (elevated)");
    TEST_ASSERT(contains(buf, (size_t)got, "kernel_base:"),  "has kernel_base:");
    TEST_ASSERT(contains(buf, (size_t)got, "kaslr_offset:"), "has kaslr_offset:");
    TEST_ASSERT(contains(buf, (size_t)got, "seed_source:"),  "has seed_source:");
    TEST_ASSERT(contains(buf, (size_t)got, "0x"),            "uses 0x hex prefix");

    if (c) spoor_clunk(c);
}

// #57a F1: /ctl/kernel-base discloses the live KASLR slide (I-16). Now that
// /ctl is world-reachable, that ONE leaf is gated on CAP_HOSTOWNER -- an
// unprivileged caller (a logged-in user, stripped of the elevation-only caps
// at rfork) cannot read it and defeat KASLR. The predicate is leaf-specific;
// the coarse procs/memory/devices/sched stats stay world-readable.
// (The format test above passes only because it temporarily elevates the test
// thread to CAP_HOSTOWNER; kproc's CAP_ALL does NOT include the elevation-only
// CAP_HOSTOWNER -- caps.h pins CAP_ALL & CAP_ELEVATION_ONLY == 0.)
void test_devctl_kernel_base_gated(void) {
    extern bool devctl_kernel_base_readable(const struct Proc *caller);

    struct Proc admin, user;
    for (size_t i = 0; i < sizeof(admin); i++) ((u8 *)&admin)[i] = 0;
    for (size_t i = 0; i < sizeof(user);  i++) ((u8 *)&user)[i]  = 0;
    admin.caps = CAP_HOSTOWNER;
    user.caps  = CAP_NONE;

    TEST_ASSERT(devctl_kernel_base_readable(&admin),
                "CAP_HOSTOWNER reads /ctl/kernel-base");
    TEST_ASSERT(!devctl_kernel_base_readable(&user),
                "F1: an unprivileged caller is denied the KASLR slide");
    TEST_ASSERT(!devctl_kernel_base_readable(NULL),
                "NULL caller denied");
}

void test_devctl_read_sched_format(void) {
    struct Spoor *c = open_ctl_leaf("sched");
    TEST_ASSERT(c != NULL, "open /ctl/sched");

    char buf[128];
    long got = devctl.read(c, buf, 128, 0);
    TEST_ASSERT(got > 0, "sched read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "runnable:"), "has runnable:");

    spoor_clunk(c);
}

void test_devctl_write_rejected(void) {
    struct Spoor *c = open_ctl_leaf("procs");
    TEST_ASSERT(c != NULL, "open /ctl/procs");

    const char cmd[] = "kill all";
    long n = (long)sizeof(cmd) - 1;
    TEST_EXPECT_EQ(devctl.write(c, cmd, n, 0), (long)-1,
                   "v1.0 ctl writes rejected (admin commands deferred)");

    spoor_clunk(c);
}

void test_devctl_read_dir_returns_neg1(void) {
    struct Spoor *root = devctl.attach("");
    TEST_ASSERT(devctl.open(root, 0) != NULL, "open root");

    char buf[16];
    TEST_EXPECT_EQ(devctl.read(root, buf, 16, 0), (long)-1,
                   "directory read returns -1 (readdir deferred)");

    spoor_clunk(root);
}

// stat_native: the apex is a directory, the leaves are regular files, and the
// FILE-TYPE bits are present (VIVARIUM V-4b-5). /ctl had no stat_native at all,
// so spoor_stat_native returned -1 -> EIO for stat("/ctl") AND for realpath()
// of anything under it (musl's resolver walks each prefix and treats any errno
// but EINVAL as fatal).
void test_devctl_stat_native_shapes(void) {
    struct t_stat st;

    struct Spoor *root = devctl.attach("");
    TEST_ASSERT(root != NULL, "attach /ctl");
    TEST_ASSERT(devctl.stat_native != NULL, "/ctl has a stat_native slot");
    TEST_EXPECT_EQ(devctl.stat_native(root, &st), 0, "stat_native(/ctl) ok");
    TEST_EXPECT_EQ(st.mode & (u32)T_S_IFMT, (u32)T_S_IFDIR,
                   "/ctl S_IFMT = S_IFDIR (S_ISDIR is true)");
    TEST_EXPECT_EQ(st.mode & ~(u32)T_S_IFMT, (u32)0555u, "/ctl perms = 0555");
    TEST_EXPECT_EQ(st.qid_type, QTDIR,            "/ctl is QTDIR");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "/ctl uid = SYSTEM");
    TEST_EXPECT_EQ(st.gid, (u32)GID_SYSTEM,       "/ctl gid = SYSTEM");
    spoor_unref(root);

    struct Spoor *procs = open_ctl_leaf("procs");
    TEST_ASSERT(procs != NULL, "open /ctl/procs");
    TEST_EXPECT_EQ(devctl.stat_native(procs, &st), 0, "stat_native(procs) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0444u), "procs = S_IFREG|0444");
    TEST_EXPECT_EQ(st.qid_type, QTFILE,               "procs is QTFILE");
    // Generated at read time from the live process table, so no size can be
    // promised in advance -- a caller that fstat'd, malloc'd, and read exactly
    // that many bytes would truncate a table that grew in between. Linux
    // reports 0 for /proc/meminfo for the same reason.
    TEST_EXPECT_EQ(st.size, (u64)0, "a generated report advertises no size");
    spoor_clunk(procs);

    // The mode DOCUMENTS the read-site gate: kernel-base needs CAP_HOSTOWNER
    // (#57a F1 -- it discloses the live KASLR slide), so advertising it
    // world-readable would have the mode lie about a file most callers cannot
    // in fact read.
    struct Spoor *kb = open_ctl_leaf("kernel-base");
    TEST_ASSERT(kb != NULL, "open /ctl/kernel-base");
    TEST_EXPECT_EQ(devctl.stat_native(kb, &st), 0, "stat_native(kernel-base) ok");
    TEST_EXPECT_EQ(st.mode, (u32)(T_S_IFREG | 0400u),
                   "kernel-base = S_IFREG|0400 (the CAP_HOSTOWNER gate, stated)");
    spoor_clunk(kb);
}
