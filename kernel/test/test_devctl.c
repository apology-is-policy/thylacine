// /ctl Dev tests (P4-D).
//
// Covers registration, walks, per-leaf reads, write rejection.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

void test_devctl_bestiary_smoke(void);
void test_devctl_attach_returns_dir(void);
void test_devctl_walk_to_each_leaf(void);
void test_devctl_walk_unknown_misses(void);
void test_devctl_read_procs_format(void);
void test_devctl_read_memory_format(void);
void test_devctl_read_devices_format(void);
void test_devctl_read_kernel_base_format(void);
void test_devctl_read_sched_format(void);
void test_devctl_write_rejected(void);
void test_devctl_read_dir_returns_neg1(void);

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
        "procs", "memory", "devices", "kernel-base", "sched",
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
    TEST_ASSERT(contains(buf, (size_t)got, "STATE"),   "header has STATE");
    TEST_ASSERT(contains(buf, (size_t)got, "ALIVE"),   "kproc shows ALIVE");

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
    struct Spoor *c = open_ctl_leaf("kernel-base");
    TEST_ASSERT(c != NULL, "open /ctl/kernel-base");

    char buf[256];
    long got = devctl.read(c, buf, 256, 0);
    TEST_ASSERT(got > 0, "kernel-base read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "kernel_base:"),  "has kernel_base:");
    TEST_ASSERT(contains(buf, (size_t)got, "kaslr_offset:"), "has kaslr_offset:");
    TEST_ASSERT(contains(buf, (size_t)got, "seed_source:"),  "has seed_source:");
    TEST_ASSERT(contains(buf, (size_t)got, "0x"),            "uses 0x hex prefix");

    spoor_clunk(c);
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
