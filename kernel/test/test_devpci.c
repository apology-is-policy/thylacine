// devpci tests (Menagerie 6b) -- the kernel-mediated PCI topology as a tree.
//
// Runs against the boot PCI enumeration (virtio_pci_init -> g_virtio_pci_devs[]).
// Under the test/boot QEMU-virt config a virtio-net-pci function is present (the
// netdev-pci probe claims it), so virtio_pci_dev_count() is normally >= 1; the
// per-function tests guard on count > 0 so the suite stays correct on a config
// with no PCI device (the root + read-only + reuse-nc paths still run).
//
// The load-bearing properties: devpci is READ-ONLY (no config-space write / no
// raw ECAM -- the I-5 posture), its ctl line is well-formed + bounded, and the
// reuse-nc walk shape the /hw/pci mount-cross depends on (the #57a contract).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/proc.h>        // PRINCIPAL_SYSTEM / GID_SYSTEM
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>     // struct t_stat + T_S_IFDIR / T_S_IFREG
#include <thylacine/types.h>
#include <thylacine/virtio_pci.h>  // virtio_pci_dev_count

void test_devpci_bestiary_smoke(void);
void test_devpci_attach_root(void);
void test_devpci_walk_read_ctl(void);
void test_devpci_readonly(void);
void test_devpci_readdir(void);
void test_devpci_walk_reuse_nc(void);

// =============================================================================
// Helpers.
// =============================================================================

// Walk one component from `from` (nc = NULL, the direct-call shape). Returns the
// resulting Spoor (caller spoor_unref's) or NULL on a miss. Does NOT consume
// `from`.
static struct Spoor *walk1(struct Spoor *from, const char *name) {
    if (!from) return NULL;
    const char *names[1] = { name };
    struct Walkqid *wq = devpci.walk(from, NULL, names, 1);
    if (!wq) return NULL;
    if (wq->nqid != 1) { spoor_unref(wq->spoor); walkqid_free(wq); return NULL; }
    struct Spoor *r = wq->spoor;
    walkqid_free(wq);
    return r;
}

// The dirent cookie/name-length/size, as test_devhw's dirent_parse.
static long dirent_parse(const u8 *buf, long pos, long len,
                         u64 *out_cookie, u32 *out_nlen, u8 *out_qtype) {
    if (pos + 24 > len) return 0;
    u64 cookie = 0;
    for (int i = 0; i < 8; i++) cookie |= (u64)buf[pos + 13 + i] << (8 * i);
    u32 nlen = (u32)buf[pos + 22] | ((u32)buf[pos + 23] << 8);
    long entry = 24 + (long)nlen;
    if (pos + entry > len) return 0;
    if (out_cookie) *out_cookie = cookie;
    if (out_nlen)   *out_nlen   = nlen;
    if (out_qtype)  *out_qtype  = buf[pos + 0];
    return entry;
}

// Does `buf[0..len)` contain the NUL-terminated `needle`?
static bool buf_contains(const u8 *buf, long len, const char *needle) {
    long nl = 0;
    while (needle[nl]) nl++;
    if (nl == 0) return true;
    for (long i = 0; i + nl <= len; i++) {
        long j = 0;
        while (j < nl && buf[i + j] == (u8)needle[j]) j++;
        if (j == nl) return true;
    }
    return false;
}

// Copy the first dirent's name out of a readdir buffer into `out` (NUL-term).
static bool first_dirent_name(const u8 *buf, long len, char *out, size_t cap) {
    u64 ck; u32 nl; u8 qt;
    long e = dirent_parse(buf, 0, len, &ck, &nl, &qt);
    if (e == 0 || nl == 0 || (size_t)nl >= cap) return false;
    for (u32 i = 0; i < nl; i++) out[i] = (char)buf[24 + i];
    out[nl] = '\0';
    return true;
}

// =============================================================================
// Tests.
// =============================================================================

void test_devpci_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('P'),     &devpci, "lookup 'P' = devpci");
    TEST_EXPECT_EQ(dev_lookup_by_name("pci"), &devpci, "lookup 'pci' = devpci");
    TEST_EXPECT_EQ(devpci.dc, 'P', "devpci.dc = 'P'");
    TEST_ASSERT(devpci.perm_enforced == false, "devpci is visibility-not-authority");
}

void test_devpci_attach_root(void) {
    struct Spoor *c = devpci.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");

    struct t_stat st;
    TEST_EXPECT_EQ(devpci.stat_native(c, &st), 0, "stat root OK");
    TEST_ASSERT((st.mode & T_S_IFDIR) != 0, "root mode IFDIR");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "root owned by SYSTEM");
    TEST_EXPECT_EQ(st.gid, (u32)GID_SYSTEM, "root group SYSTEM");

    spoor_unref(c);
}

// The core: root -> <bdf> dir -> ctl -> read -> a well-formed line. Skips the
// function-specific assertions on a no-PCI config (root still proven above).
void test_devpci_walk_read_ctl(void) {
    int count = virtio_pci_dev_count();
    if (count <= 0) {
        TEST_ASSERT(true, "no PCI function present -- ctl path skipped (config)");
        return;
    }

    struct Spoor *root = devpci.attach("");
    TEST_ASSERT(root != NULL, "attach root");

    // Get the first function's directory name (its bdf) from readdir.
    static u8 dbuf[1024];
    long dn = devpci.readdir(root, dbuf, sizeof(dbuf), 0);
    TEST_ASSERT(dn > 0, "root readdir non-empty (a function is present)");
    char bdf[32];
    TEST_ASSERT(first_dirent_name(dbuf, dn, bdf, sizeof(bdf)), "first bdf name parsed");

    struct Spoor *fdir = walk1(root, bdf);
    TEST_ASSERT(fdir != NULL, "walk root/<bdf> resolves");
    TEST_EXPECT_EQ(fdir->qid.type, QTDIR, "<bdf> is QTDIR");
    TEST_ASSERT(fdir->qid.path != 0, "<bdf> qid != root");

    // A function directory does not byte-read (readdir is its path).
    u8 tmp[16];
    TEST_EXPECT_EQ(devpci.read(fdir, tmp, sizeof(tmp), 0), (long)-1, "<bdf> dir read -> -1");

    struct Spoor *ctl = walk1(fdir, "ctl");
    TEST_ASSERT(ctl != NULL, "walk <bdf>/ctl resolves");
    TEST_EXPECT_EQ(ctl->qid.type, QTFILE, "ctl is QTFILE");
    TEST_ASSERT(devpci.open(ctl, 0) != NULL, "open ctl");

    u8 line[128];
    long got = devpci.read(ctl, line, sizeof(line), 0);
    TEST_ASSERT(got > 0, "ctl read returns content");
    TEST_ASSERT(buf_contains(line, got, "v1 "),     "ctl is versioned (v1)");
    TEST_ASSERT(buf_contains(line, got, "bus="),    "ctl reports bus");
    TEST_ASSERT(buf_contains(line, got, "dev="),    "ctl reports dev");
    TEST_ASSERT(buf_contains(line, got, "fn="),     "ctl reports fn");
    TEST_ASSERT(buf_contains(line, got, "vendor="), "ctl reports vendor");
    TEST_ASSERT(buf_contains(line, got, "virtio="), "ctl reports virtio id");
    TEST_ASSERT(buf_contains(line, got, "intid="),  "ctl reports intid");
    TEST_ASSERT(line[got - 1] == (u8)'\n',          "ctl line is newline-terminated");

    // EOF semantics + a stat size matching the line length.
    struct t_stat st;
    TEST_EXPECT_EQ(devpci.stat_native(ctl, &st), 0, "stat ctl OK");
    TEST_ASSERT((st.mode & T_S_IFREG) != 0, "ctl mode IFREG");
    TEST_EXPECT_EQ(st.size, (u64)got, "ctl stat size == read length");
    TEST_EXPECT_EQ(devpci.read(ctl, line, sizeof(line), got), (long)0, "read at len -> EOF");
    TEST_EXPECT_EQ(devpci.read(ctl, line, sizeof(line), got + 99), (long)0, "read past len -> EOF");

    // A ctl leaf is terminal: nothing walks from it.
    TEST_ASSERT(walk1(ctl, "anything") == NULL, "no walk descends from ctl");

    devpci.close(ctl);
    spoor_unref(ctl);
    spoor_unref(fdir);
    spoor_unref(root);
}

// I-5 / read-only: devpci NEVER writes config space and exposes no mutation
// surface. Every write / create / wstat / remove rejects, on a dir AND a ctl.
void test_devpci_readonly(void) {
    struct Spoor *root = devpci.attach("");
    TEST_ASSERT(root != NULL, "attach root");

    u8 junk[4] = { 1, 2, 3, 4 };
    TEST_EXPECT_EQ(devpci.write(root, junk, sizeof(junk), 0), (long)-1, "write root -> -1");
    TEST_ASSERT(devpci.create(root, "x", 0, 0, 0) == NULL, "create -> NULL");
    TEST_EXPECT_EQ(devpci.wstat(root, junk, sizeof(junk)), -1, "wstat -> -1");
    TEST_EXPECT_EQ(devpci.bwrite(root, NULL, 0), (long)-1, "bwrite -> -1");

    if (virtio_pci_dev_count() > 0) {
        static u8 dbuf[1024];
        long dn = devpci.readdir(root, dbuf, sizeof(dbuf), 0);
        char bdf[32];
        if (dn > 0 && first_dirent_name(dbuf, dn, bdf, sizeof(bdf))) {
            struct Spoor *fdir = walk1(root, bdf);
            if (fdir) {
                struct Spoor *ctl = walk1(fdir, "ctl");
                if (ctl) {
                    TEST_EXPECT_EQ(devpci.write(ctl, junk, sizeof(junk), 0), (long)-1,
                                   "write ctl -> -1 (no config-space write)");
                    TEST_EXPECT_EQ(devpci.wstat(ctl, junk, sizeof(junk)), -1, "wstat ctl -> -1");
                    spoor_unref(ctl);
                }
                spoor_unref(fdir);
            }
        }
    }
    spoor_unref(root);
}

// readdir: the root lists one QTDIR per function with a strictly-increasing,
// never-0 cookie (the SYS_READDIR contract); each function dir lists exactly its
// `ctl` QTFILE.
void test_devpci_readdir(void) {
    int count = virtio_pci_dev_count();
    struct Spoor *root = devpci.attach("");
    TEST_ASSERT(root != NULL, "attach root");

    static u8 buf[2048];
    long n = devpci.readdir(root, buf, sizeof(buf), 0);
    TEST_ASSERT(n >= 0, "root readdir does not error");

    int entries = 0;
    u64 prev = 0;
    bool strict = true, nonzero = true, all_dirs = true;
    long pos = 0;
    for (;;) {
        u64 ck; u32 nl; u8 qt;
        long e = dirent_parse(buf, pos, n, &ck, &nl, &qt);
        if (e == 0) break;
        if (ck == 0) nonzero = false;
        if (ck <= prev) strict = false;
        if (qt != QTDIR) all_dirs = false;
        prev = ck;
        entries++;
        pos += e;
    }
    TEST_EXPECT_EQ(entries, count, "root lists one dir per PCI function");
    TEST_ASSERT(nonzero, "every root cookie is non-zero");
    TEST_ASSERT(strict, "root cookies strictly increase");
    TEST_ASSERT(all_dirs, "every root entry is a directory");

    if (count > 0) {
        char bdf[32];
        TEST_ASSERT(first_dirent_name(buf, n, bdf, sizeof(bdf)), "first bdf parsed");
        struct Spoor *fdir = walk1(root, bdf);
        TEST_ASSERT(fdir != NULL, "walk to <bdf>");
        long fn = devpci.readdir(fdir, buf, sizeof(buf), 0);
        u64 ck; u32 nl; u8 qt;
        long e = dirent_parse(buf, 0, fn, &ck, &nl, &qt);
        TEST_ASSERT(e > 0, "function dir lists an entry");
        TEST_EXPECT_EQ(qt, (u8)QTFILE, "the entry is a file (ctl)");
        TEST_ASSERT(nl == 3 && buf[24] == 'c' && buf[25] == 't' && buf[26] == 'l',
                    "the entry is named ctl");
        // exactly one entry: resuming past it is EOD.
        TEST_EXPECT_EQ(devpci.readdir(fdir, buf, sizeof(buf), (s64)ck), (long)0,
                       "function dir has exactly one entry");
        spoor_unref(fdir);
    }
    spoor_unref(root);
}

// The reuse-nc walk path (#57a) -- the shape clone_walk_zero needs to cross the
// /hw/pci mount. devpci.walk(c, nc, ...) MUST return nc AS wq->spoor with its qid
// advanced. Proven on the root (a 0-element walk returns nc unchanged with
// nqid == 0; a 1-element walk advances nc). Function-specific only when a PCI
// device is present.
void test_devpci_walk_reuse_nc(void) {
    struct Spoor *root = devpci.attach("");
    TEST_ASSERT(root != NULL, "attach root");

    // 0-element walk: nc returned unchanged, nqid == 0 (the mount-cross shape).
    struct Spoor *nc0 = spoor_clone(root);
    TEST_ASSERT(nc0 != NULL, "clone nc (clone_walk_zero analog)");
    struct Walkqid *wq0 = devpci.walk(root, nc0, NULL, 0);
    TEST_ASSERT(wq0 != NULL, "0-element walk allocates");
    TEST_EXPECT_EQ(wq0->spoor, nc0, "reuse-nc: returned spoor IS nc");
    TEST_EXPECT_EQ(wq0->nqid, 0, "0-element walk -> nqid 0");
    TEST_EXPECT_EQ(nc0->qid.path, (u64)0, "nc unchanged at root");
    walkqid_free(wq0);
    spoor_unref(nc0);

    if (virtio_pci_dev_count() > 0) {
        static u8 dbuf[1024];
        long dn = devpci.readdir(root, dbuf, sizeof(dbuf), 0);
        char bdf[32];
        if (dn > 0 && first_dirent_name(dbuf, dn, bdf, sizeof(bdf))) {
            struct Spoor *nc1 = spoor_clone(root);
            TEST_ASSERT(nc1 != NULL, "clone nc1");
            const char *names[1] = { bdf };
            struct Walkqid *wq1 = devpci.walk(root, nc1, names, 1);
            TEST_ASSERT(wq1 != NULL, "1-element walk allocates");
            TEST_EXPECT_EQ(wq1->spoor, nc1, "reuse-nc: returned spoor IS nc1");
            TEST_EXPECT_EQ(wq1->nqid, 1, "walk to <bdf> via nc succeeds");
            TEST_ASSERT(nc1->qid.path != 0, "nc1 advanced off the root");
            walkqid_free(wq1);
            spoor_unref(nc1);
        }
    }
    spoor_unref(root);
}
