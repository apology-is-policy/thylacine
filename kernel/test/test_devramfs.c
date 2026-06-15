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
void test_devramfs_readdir_enumerates_root(void);
void test_devramfs_readdir_file_returns_neg1(void);
void test_devramfs_readdir_synth_dir_empty(void);
void test_devramfs_readdir_paginates_no_dup_no_skip(void);

// =============================================================================
// Helpers.
// =============================================================================

// Exact NUL-terminated string equality (the devramfs internal `ramfs_streq` is
// static to devramfs.c; the readdir tests need their own).
static bool name_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

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
                   "plain read() on a directory returns -1 (use readdir)");

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

// =============================================================================
// readdir (U-6e-b-1). The boot ramfs enumerates its flat root: every cpio file
// (QTFILE) plus the synthetic mount-point dirs srv/proc (QTDIR). Wire format per
// entry: qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name -- the same
// the SYS_READDIR handler parses. These tests drive devramfs.readdir directly
// and (for pagination) emulate the handler's resume-cookie round-trip.
// =============================================================================

// Parse one entry at buf[pos..got). Returns the entry byte-length, or 0 if there
// is no complete entry at pos. Fills out_qtype (qid.type byte 0), out_cookie
// (the 8 LE resume cookie), and a NUL-terminated name copy.
static long ramfs_de_parse(const u8 *buf, long got, long pos,
                           u8 *out_qtype, u64 *out_cookie,
                           char *namebuf, size_t namecap) {
    if (pos + 24 > got) return 0;
    u32 nlen = (u32)buf[pos + 22] | ((u32)buf[pos + 23] << 8);
    if (pos + 24 + (long)nlen > got) return 0;
    if (out_qtype)  *out_qtype = buf[pos + 0];
    if (out_cookie) {
        u64 ck = 0;
        for (int i = 0; i < 8; i++) ck |= (u64)buf[pos + 13 + i] << (8 * i);
        *out_cookie = ck;
    }
    if (namebuf && namecap > 0) {
        size_t k = 0;
        for (; k < (size_t)nlen && k + 1 < namecap; k++) namebuf[k] = (char)buf[pos + 24 + k];
        namebuf[k] = '\0';
    }
    return 24 + (long)nlen;
}

// Whether the readdir run in buf[0..got) contains an entry named `want`, and if
// so its qid.type (via out_qtype).
static bool ramfs_de_run_has(const u8 *buf, long got, const char *want, u8 *out_qtype) {
    long pos = 0;
    char name[128];
    u8 qt;
    for (;;) {
        long len = ramfs_de_parse(buf, got, pos, &qt, NULL, name, sizeof(name));
        if (len == 0) break;
        if (name_eq(name, want)) {
            if (out_qtype) *out_qtype = qt;
            return true;
        }
        pos += len;
    }
    return false;
}

void test_devramfs_readdir_enumerates_root(void) {
    if (devramfs_file_count() < 2) return;       // initrd absent

    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(root != NULL, "attach root");
    TEST_ASSERT(devramfs.readdir != NULL, "devramfs has .readdir (U-6e-b-1)");
    TEST_ASSERT(devramfs.open(root, 0) != NULL, "open root");

    // One large-buffer call returns the whole flat root (the live corpus is
    // ~70 short entries, far under 8 KiB).
    static u8 buf[8192];
    long got = devramfs.readdir(root, buf, (long)sizeof(buf), 0);
    TEST_ASSERT(got > 0, "readdir root returns bytes");

    u8 qt = 0;
    TEST_ASSERT(ramfs_de_run_has(buf, got, "welcome", &qt), "root lists 'welcome'");
    TEST_EXPECT_EQ((u64)qt, (u64)QTFILE, "welcome is QTFILE");
    TEST_ASSERT(ramfs_de_run_has(buf, got, "version", &qt), "root lists 'version'");
    TEST_EXPECT_EQ((u64)qt, (u64)QTFILE, "version is QTFILE");
    // The synthetic mount-point dirs are enumerated and typed as directories.
    TEST_ASSERT(ramfs_de_run_has(buf, got, "srv", &qt), "root lists 'srv'");
    TEST_EXPECT_EQ((u64)qt, (u64)QTDIR, "srv is QTDIR");
    TEST_ASSERT(ramfs_de_run_has(buf, got, "proc", &qt), "root lists 'proc'");
    TEST_EXPECT_EQ((u64)qt, (u64)QTDIR, "proc is QTDIR");
    TEST_ASSERT(ramfs_de_run_has(buf, got, "ctl", &qt), "root lists 'ctl'");
    TEST_EXPECT_EQ((u64)qt, (u64)QTDIR, "ctl is QTDIR");

    spoor_clunk(root);
}

void test_devramfs_readdir_file_returns_neg1(void) {
    if (devramfs_file_count() < 1) return;

    struct Spoor *c = open_ramfs_file("welcome");
    TEST_ASSERT(c != NULL, "open welcome");
    char buf[64];
    TEST_EXPECT_EQ(devramfs.readdir(c, buf, (long)sizeof(buf), 0), (long)-1,
                   "readdir on a regular file returns -1 (not a directory)");
    spoor_clunk(c);
}

void test_devramfs_readdir_buffer_too_small_errs(void) {
    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(root != NULL, "attach root");
    TEST_ASSERT(devramfs.open(root, 0) != NULL, "open root");
    // 16 bytes can't hold even a 24-byte dirent header, so the first entry
    // never fits -> -1 ("buffer too small"), NOT 0 (which would be a silently
    // truncating EOD).
    u8 tiny[16];
    TEST_EXPECT_EQ(devramfs.readdir(root, tiny, (long)sizeof(tiny), 0), (long)-1,
                   "buffer too small for the first entry returns -1 (not EOD)");
    spoor_clunk(root);
}

void test_devramfs_readdir_synth_dir_empty(void) {
    struct Spoor *root = devramfs.attach("");
    struct Spoor *srv = walk_one(root, "srv");
    TEST_ASSERT(srv != NULL, "walk('srv') -> synth dir");
    TEST_EXPECT_EQ(srv->qid.type, QTDIR, "srv is a directory");
    char buf[64];
    TEST_EXPECT_EQ(devramfs.readdir(srv, buf, (long)sizeof(buf), 0), (long)0,
                   "synthetic mount-point dir is empty (0 == EOD)");
    spoor_unref(srv);

    // /ctl is a synth mount point too (#57): present, a directory, empty.
    struct Spoor *ctl = walk_one(root, "ctl");
    TEST_ASSERT(ctl != NULL, "walk('ctl') -> synth dir");
    TEST_EXPECT_EQ(ctl->qid.type, QTDIR, "ctl is a directory");
    TEST_EXPECT_EQ(devramfs.readdir(ctl, buf, (long)sizeof(buf), 0), (long)0,
                   "ctl synthetic mount-point dir is empty (0 == EOD)");
    spoor_unref(ctl);

    // /dev is a synth mount point too (#57b): present, a directory, empty.
    struct Spoor *dev = walk_one(root, "dev");
    spoor_unref(root);
    TEST_ASSERT(dev != NULL, "walk('dev') -> synth dir");
    TEST_EXPECT_EQ(dev->qid.type, QTDIR, "dev is a directory");
    TEST_EXPECT_EQ(devramfs.readdir(dev, buf, (long)sizeof(buf), 0), (long)0,
                   "dev synthetic mount-point dir is empty (0 == EOD)");
    spoor_unref(dev);
}

// The resume cookie must let a small buffer paginate the whole root with no
// duplicated and no skipped entry -- the property the SYS_READDIR handler relies
// on (it stores the last entry's cookie into c->offset for the next call). Here
// we emulate that round-trip with a buffer sized for only 1-2 short entries.
void test_devramfs_readdir_paginates_no_dup_no_skip(void) {
    if (devramfs_file_count() < 2) return;

    struct Spoor *root = devramfs.attach("");
    TEST_ASSERT(root != NULL, "attach root");
    TEST_ASSERT(devramfs.open(root, 0) != NULL, "open root");

    // Sized to hold the longest boot-corpus entry (24-byte header + the longest
    // cpio name, ~28 chars) yet small enough to force many runs over the ~70
    // entries -- the resume-cookie path is exercised across run boundaries
    // without tripping the "first entry too small" error.
    u8 buf[96];
    s64 off = 0;                 // start cookie
    u64 prev_last = 0;           // last cookie seen (monotonic check)
    int  count = 0;
    bool saw_welcome = false, saw_srv = false, saw_ctl = false, saw_dev = false,
         saw_hw = false;
    bool monotonic = true, well_formed = true;

    for (int guard = 0; guard < 4096; guard++) {  // guard against a non-advancing bug
        long got = devramfs.readdir(root, buf, (long)sizeof(buf), off);
        if (got < 0) { well_formed = false; break; }
        if (got == 0) break;                       // EOD
        // Parse every complete entry in the run; the run must hold >= 1.
        long pos = 0; int in_run = 0; u64 last_cookie = (u64)off;
        char name[128]; u8 qt; u64 ck;
        for (;;) {
            long len = ramfs_de_parse(buf, got, pos, &qt, &ck, name, sizeof(name));
            if (len == 0) break;
            if (ck <= prev_last) monotonic = false;  // strictly increasing across the whole walk
            prev_last = ck;
            last_cookie = ck;
            if (name_eq(name, "welcome")) saw_welcome = true;
            if (name_eq(name, "srv"))     saw_srv = true;
            if (name_eq(name, "ctl"))     saw_ctl = true;
            if (name_eq(name, "dev"))     saw_dev = true;
            if (name_eq(name, "hw"))      saw_hw = true;
            count++;
            in_run++;
            pos += len;
        }
        if (in_run == 0) { well_formed = false; break; }  // a run with no complete entry would spin
        off = (s64)last_cookie;                            // resume after the last entry (handler semantics)
    }

    TEST_ASSERT(well_formed, "every non-empty run holds >= 1 complete entry");
    TEST_ASSERT(monotonic, "resume cookies strictly increase (no dup, no rewind)");
    TEST_ASSERT(saw_welcome, "paginated walk still finds 'welcome'");
    TEST_ASSERT(saw_srv, "paginated walk still finds the 'srv' synth dir");
    TEST_ASSERT(saw_ctl, "paginated walk still finds the 'ctl' synth dir");
    TEST_ASSERT(saw_dev, "paginated walk still finds the 'dev' synth dir (#57b)");
    TEST_ASSERT(saw_hw, "paginated walk still finds the 'hw' synth dir (devhw)");
    // Total enumerated == files + the synthetic mount-point dirs. Derive the
    // synth count (devramfs_synth_dir_count) rather than hardcode it, so adding
    // a future synth dir does not silently break this no-skip/no-dup assertion.
    TEST_EXPECT_EQ((u64)count, (u64)(devramfs_file_count() + devramfs_synth_dir_count()),
                   "paginated count == files + synth dirs (no skip, no dup)");

    spoor_clunk(root);
}
