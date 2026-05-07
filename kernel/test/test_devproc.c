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

#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
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
void test_devproc_write_ctl_consumes(void);
void test_devproc_read_dir_returns_neg1(void);
void test_devproc_read_partial_offset(void);

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

void test_devproc_write_ctl_consumes(void) {
    struct Spoor *root = devproc.attach("");
    struct Spoor *piddir = walk_one(root, "0");
    struct Spoor *ctl = walk_one(piddir, "ctl");
    spoor_unref(piddir);
    spoor_unref(root);
    TEST_ASSERT(ctl != NULL, "walk to /proc/0/ctl OK");
    TEST_ASSERT(devproc.open(ctl, 0) != NULL, "open ctl");

    const char cmd[] = "kill";
    long n = (long)sizeof(cmd) - 1;
    long w = devproc.write(ctl, cmd, n, 0);
    TEST_EXPECT_EQ(w, n, "ctl write returns n at v1.0 (stub: command consumed)");

    spoor_clunk(ctl);

    // Writes to non-ctl files (e.g., status) are rejected.
    struct Spoor *status = open_status_for_pid(0);
    TEST_ASSERT(status != NULL, "open status");
    TEST_EXPECT_EQ(devproc.write(status, cmd, n, 0), (long)-1,
                   "writes to status return -1");
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
