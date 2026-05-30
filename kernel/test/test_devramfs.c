// devramfs Dev tests (P4-E).
//
// Tests against the actual ramfs initrd loaded by QEMU at boot. The
// initrd contains 'welcome' and 'version' text files (per
// tools/build.sh::build_ramfs).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>

void test_devramfs_bestiary_smoke(void);
void test_devramfs_initialized_with_files(void);
void test_devramfs_attach_returns_dir(void);
void test_devramfs_walk_to_welcome(void);
void test_devramfs_walk_unknown_misses(void);
void test_devramfs_read_welcome(void);
void test_devramfs_read_version(void);
void test_devramfs_read_partial_offset(void);
void test_devramfs_read_dir_returns_neg1(void);
void test_devramfs_write_rejected(void);
void test_devramfs_stat_native_system_owned(void);

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
    struct Walkqid *wq = devramfs.walk(c, NULL, names, 1);
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

static struct Spoor *open_ramfs_file(const char *name) {
    struct Spoor *root = devramfs.attach("");
    if (!root) return NULL;
    struct Spoor *file = walk_one(root, name);
    spoor_unref(root);
    if (!file) return NULL;
    if (!devramfs.open(file, 0)) {
        spoor_unref(file);
        return NULL;
    }
    return file;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devramfs_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('m'),         &devramfs, "lookup 'm' = devramfs");
    TEST_EXPECT_EQ(dev_lookup_by_name("ramfs"),   &devramfs, "lookup 'ramfs' = devramfs");
    TEST_EXPECT_EQ(devramfs.dc, 'm',                         "devramfs.dc = 'm'");
}

void test_devramfs_initialized_with_files(void) {
    TEST_ASSERT(devramfs_initialized(),
                "devramfs init must have run during dev_init");
    // QEMU -initrd path: tools/build.sh writes 2 files (welcome,
    // version). On a guest without -initrd, the count is 0 and the
    // remaining tests skip.
    int n = devramfs_file_count();
    TEST_ASSERT(n >= 2,
                "expected at least 2 files from initrd (welcome + version)");
}

void test_devramfs_attach_returns_dir(void) {
    struct Spoor *c = devramfs.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devramfs_walk_to_welcome(void) {
    if (devramfs_file_count() < 1) return;       // initrd absent

    struct Spoor *root = devramfs.attach("");
    struct Spoor *welcome = walk_one(root, "welcome");
    spoor_unref(root);
    TEST_ASSERT(welcome != NULL, "walk('welcome') succeeds");
    TEST_EXPECT_EQ(welcome->qid.type, QTFILE, "welcome is QTFILE");
    spoor_unref(welcome);
}

void test_devramfs_walk_unknown_misses(void) {
    struct Spoor *root = devramfs.attach("");
    const char *names[1] = { "no-such-file" };
    struct Walkqid *wq = devramfs.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "unknown name misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);
    spoor_unref(root);
}

void test_devramfs_read_welcome(void) {
    if (devramfs_file_count() < 1) return;

    struct Spoor *c = open_ramfs_file("welcome");
    TEST_ASSERT(c != NULL, "open welcome");

    char buf[128];
    long got = devramfs.read(c, buf, 128, 0);
    TEST_ASSERT(got > 0, "welcome read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "Welcome"),
                "welcome contains 'Welcome'");
    TEST_ASSERT(contains(buf, (size_t)got, "Thylacine"),
                "welcome contains 'Thylacine'");

    spoor_clunk(c);
}

void test_devramfs_read_version(void) {
    if (devramfs_file_count() < 2) return;

    struct Spoor *c = open_ramfs_file("version");
    TEST_ASSERT(c != NULL, "open version");

    char buf[64];
    long got = devramfs.read(c, buf, 64, 0);
    TEST_ASSERT(got > 0, "version read positive");
    TEST_ASSERT(contains(buf, (size_t)got, "Thylacine"),
                "version contains 'Thylacine'");
    TEST_ASSERT(contains(buf, (size_t)got, "0.1-dev"),
                "version contains '0.1-dev'");

    spoor_clunk(c);
}

void test_devramfs_read_partial_offset(void) {
    if (devramfs_file_count() < 1) return;

    struct Spoor *c = open_ramfs_file("welcome");
    TEST_ASSERT(c != NULL, "open welcome");

    char full[128];
    long full_n = devramfs.read(c, full, 128, 0);
    TEST_ASSERT(full_n > 5, "full read at least 5 bytes");

    char partial[16];
    long got = devramfs.read(c, partial, 4, 2);
    TEST_ASSERT(got > 0, "partial read positive");
    TEST_ASSERT(got <= 4, "partial read bounded by n");
    for (long i = 0; i < got; i++) {
        TEST_ASSERT(partial[i] == full[2 + i],
                    "partial slice matches the corresponding window");
    }

    long eof = devramfs.read(c, partial, 16, full_n + 100);
    TEST_EXPECT_EQ(eof, (long)0, "off > total returns 0 (EOF)");

    spoor_clunk(c);
}

void test_devramfs_read_dir_returns_neg1(void) {
    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(devramfs.open(root, 0) != NULL, "open root");

    char buf[16];
    TEST_EXPECT_EQ(devramfs.read(root, buf, 16, 0), (long)-1,
                   "directory read returns -1 (readdir deferred)");

    spoor_clunk(root);
}

void test_devramfs_write_rejected(void) {
    if (devramfs_file_count() < 1) return;

    struct Spoor *c = open_ramfs_file("welcome");
    TEST_ASSERT(c != NULL, "open welcome");

    const char data[] = "garbage";
    long n = (long)sizeof(data) - 1;
    TEST_EXPECT_EQ(devramfs.write(c, data, n, 0), (long)-1,
                   "writes to ramfs files rejected (read-only)");

    spoor_clunk(c);
}

// A-2a: the read-only boot FS reports every entry as system-owned
// (PRINCIPAL_SYSTEM / GID_SYSTEM) -- there is no per-file owner table in the
// cpio ramfs. Covers both branches of devramfs_stat_native (root dir + file).
void test_devramfs_stat_native_system_owned(void) {
    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(root != NULL, "attach root");
    TEST_ASSERT(devramfs.stat_native != NULL, "devramfs has .stat_native");

    struct t_stat st;
    TEST_EXPECT_EQ((u64)devramfs.stat_native(root, &st), (u64)0,
                    "root stat_native -> 0");
    TEST_EXPECT_EQ((u64)st.uid, (u64)PRINCIPAL_SYSTEM, "root uid = SYSTEM");
    TEST_EXPECT_EQ((u64)st.gid, (u64)GID_SYSTEM,       "root gid = SYSTEM");
    TEST_ASSERT((st.mode & T_S_IFMT) == T_S_IFDIR,     "root is a directory");

    if (devramfs_file_count() >= 1) {
        struct Spoor *f = walk_one(root, "welcome");
        TEST_ASSERT(f != NULL, "walk welcome");
        TEST_EXPECT_EQ((u64)devramfs.stat_native(f, &st), (u64)0,
                        "file stat_native -> 0");
        TEST_EXPECT_EQ((u64)st.uid, (u64)PRINCIPAL_SYSTEM, "file uid = SYSTEM");
        TEST_EXPECT_EQ((u64)st.gid, (u64)GID_SYSTEM,       "file gid = SYSTEM");
        TEST_ASSERT((st.mode & T_S_IFMT) == T_S_IFREG,     "file is regular");
        spoor_unref(f);
    }

    spoor_unref(root);
}
