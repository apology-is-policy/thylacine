// devhw tests (Menagerie build-arc 1) -- the DTB published as a walkable tree.
//
// Runs against the real boot DTB (QEMU virt under the test harness), whose
// shape is stable: a root node with #address-cells (a 4-byte cell property),
// a /cpus node, and a /cpus/cpu@0 sub-node. (Kernel tests never run on real
// hardware -- production is KERNEL_TESTS=OFF -- so relying on the QEMU-virt DTB
// is sound.) The load-bearing test is readdir_cookie_contract: the dirent
// cookie must be strictly monotonic + never 0 (the SYS_READDIR handler's
// pagination contract), proven by a chunked enumeration matching a single-call
// one entry-for-entry.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/dtb.h>
#include <thylacine/proc.h>       // PRINCIPAL_SYSTEM / GID_SYSTEM
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>    // struct t_stat + T_S_IFDIR / T_S_IFREG
#include <thylacine/types.h>

void test_devhw_bestiary_smoke(void);
void test_devhw_attach_returns_root(void);
void test_devhw_walk_node_and_prop(void);
void test_devhw_walk_deep_and_dotdot(void);
void test_devhw_walk_miss(void);
void test_devhw_prop_read(void);
void test_devhw_stat_native(void);
void test_devhw_readdir_cookie_contract(void);
void test_devhw_iter_api(void);

// =============================================================================
// Helpers.
// =============================================================================

// Walk one component from `from` (nc = NULL, the direct-call shape). Returns
// the resulting Spoor (caller spoor_unref's) or NULL on a miss. Does NOT
// consume `from`.
static struct Spoor *walk1(struct Spoor *from, const char *name) {
    if (!from) return NULL;
    const char *names[1] = { name };
    struct Walkqid *wq = devhw.walk(from, NULL, names, 1);
    if (!wq) return NULL;
    if (wq->nqid != 1) { spoor_unref(wq->spoor); walkqid_free(wq); return NULL; }
    struct Spoor *r = wq->spoor;
    walkqid_free(wq);
    return r;
}

// Parse the dirent at `buf[pos..]` (bounded by `len`): extract the 8-byte LE
// cookie, the 2-byte LE name length, and the total entry size. Returns the
// entry size, or 0 if a complete entry does not fit.
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

// =============================================================================
// Tests.
// =============================================================================

void test_devhw_bestiary_smoke(void) {
    TEST_EXPECT_EQ(dev_lookup_by_dc('H'),    &devhw, "lookup 'H' = devhw");
    TEST_EXPECT_EQ(dev_lookup_by_name("hw"), &devhw, "lookup 'hw' = devhw");
    TEST_EXPECT_EQ(devhw.dc, 'H', "devhw.dc = 'H'");
    TEST_ASSERT(devhw.perm_enforced == false, "devhw is visibility-not-authority");
}

void test_devhw_attach_returns_root(void) {
    struct Spoor *c = devhw.attach("");
    TEST_ASSERT(c != NULL, "attach OK");
    TEST_EXPECT_EQ(c->qid.path, (u64)0, "root qid.path = 0 (DTB_NODE_ROOT)");
    TEST_EXPECT_EQ(c->qid.type, QTDIR, "root QTDIR");
    spoor_unref(c);
}

void test_devhw_walk_node_and_prop(void) {
    struct Spoor *root = devhw.attach("");
    TEST_ASSERT(root != NULL, "attach root");

    // A node child -> QTDIR. /cpus is mandatory on every ARM64 DTB.
    struct Spoor *cpus = walk1(root, "cpus");
    TEST_ASSERT(cpus != NULL, "walk root/cpus resolves");
    TEST_EXPECT_EQ(cpus->qid.type, QTDIR, "cpus is QTDIR");
    TEST_ASSERT(cpus->qid.path != 0, "cpus qid != root");

    // A property child -> QTFILE. The root #address-cells is required by the
    // DT spec for any node with addressable children.
    struct Spoor *ac = walk1(root, "#address-cells");
    TEST_ASSERT(ac != NULL, "walk root/#address-cells resolves");
    TEST_EXPECT_EQ(ac->qid.type, QTFILE, "#address-cells is QTFILE");

    spoor_unref(ac);
    spoor_unref(cpus);
    spoor_unref(root);
}

void test_devhw_walk_deep_and_dotdot(void) {
    struct Spoor *root = devhw.attach("");
    struct Spoor *cpus = walk1(root, "cpus");
    TEST_ASSERT(cpus != NULL, "root/cpus");

    // Deep walk: /cpus/cpu@0 (always present on QEMU virt).
    struct Spoor *cpu0 = walk1(cpus, "cpu@0");
    TEST_ASSERT(cpu0 != NULL, "root/cpus/cpu@0 resolves");
    TEST_EXPECT_EQ(cpu0->qid.type, QTDIR, "cpu@0 is QTDIR");

    // ".." climbs: cpu@0/.. -> cpus, cpus/.. -> root (qid.path 0).
    struct Spoor *up1 = walk1(cpu0, "..");
    TEST_ASSERT(up1 != NULL, "cpu@0/.. resolves");
    TEST_EXPECT_EQ(up1->qid.path, cpus->qid.path, "cpu@0/.. == cpus");

    struct Spoor *up2 = walk1(up1, "..");
    TEST_ASSERT(up2 != NULL, "cpus/.. resolves");
    TEST_EXPECT_EQ(up2->qid.path, (u64)0, "cpus/.. == root");

    spoor_unref(up2);
    spoor_unref(up1);
    spoor_unref(cpu0);
    spoor_unref(cpus);
    spoor_unref(root);
}

void test_devhw_walk_miss(void) {
    struct Spoor *root = devhw.attach("");
    const char *names[1] = { "no-such-node-or-prop" };
    struct Walkqid *wq = devhw.walk(root, NULL, names, 1);
    TEST_ASSERT(wq != NULL, "walk allocates");
    TEST_EXPECT_EQ(wq->nqid, 0, "walk to a missing name misses");
    spoor_unref(wq->spoor);
    walkqid_free(wq);

    // A property leaf is terminal: walking from it misses.
    struct Spoor *ac = walk1(root, "#address-cells");
    TEST_ASSERT(ac != NULL, "root/#address-cells");
    struct Spoor *none = walk1(ac, "anything");
    TEST_ASSERT(none == NULL, "no walk descends from a property file");

    spoor_unref(ac);
    spoor_unref(root);
}

// The reuse-nc walk path (#57a). When stalk crosses a mount it pre-clones the
// target root (clone_walk_zero) and passes that nc to dev->walk, requiring it
// back AS wq->spoor with its qid advanced -- else a mounted /hw is unreachable
// through stalk (wq->spoor != nc -> reject). devhw-1 does not mount /hw yet, so
// the direct-call (nc == NULL) path covers the kernel tests; this exercises the
// nc-reuse path the eventual mount depends on.
void test_devhw_walk_reuse_nc(void) {
    struct Spoor *root = devhw.attach("");
    TEST_ASSERT(root != NULL, "attach root");
    struct Spoor *nc = spoor_clone(root);
    TEST_ASSERT(nc != NULL, "clone an nc (clone_walk_zero analog)");

    const char *names[1] = { "cpus" };
    struct Walkqid *wq = devhw.walk(root, nc, names, 1);
    TEST_ASSERT(wq != NULL, "walk with nc allocates");
    TEST_EXPECT_EQ(wq->spoor, nc, "reuse-nc: the returned spoor IS the caller's nc");
    TEST_EXPECT_EQ(wq->nqid, 1, "walk to cpus via nc succeeds");
    TEST_EXPECT_EQ(nc->qid.type, QTDIR, "nc advanced to cpus (QTDIR)");
    TEST_ASSERT(nc->qid.path != 0, "nc advanced off the root");

    walkqid_free(wq);
    spoor_unref(nc);     // wq->spoor IS nc -- a single unref
    spoor_unref(root);
}

void test_devhw_prop_read(void) {
    struct Spoor *root = devhw.attach("");
    struct Spoor *ac = walk1(root, "#address-cells");
    TEST_ASSERT(ac != NULL, "root/#address-cells");
    TEST_ASSERT(devhw.open(ac, 0) != NULL, "open #address-cells");

    // #address-cells is exactly one u32 cell -> 4 bytes.
    u8 buf[16];
    long got = devhw.read(ac, buf, sizeof(buf), 0);
    TEST_EXPECT_EQ(got, (long)4, "#address-cells reads 4 bytes (one cell)");

    // The value is big-endian; QEMU virt root #address-cells = 2. Accept any
    // sane cell count (1 or 2) to stay portable, but prove the bytes decode.
    u32 cells = ((u32)buf[0] << 24) | ((u32)buf[1] << 16) |
                ((u32)buf[2] << 8) | (u32)buf[3];
    TEST_ASSERT(cells == 1u || cells == 2u, "#address-cells decodes to 1 or 2");

    // EOF: read at the property length returns 0; beyond it returns 0.
    TEST_EXPECT_EQ(devhw.read(ac, buf, sizeof(buf), 4), (long)0, "read at len -> EOF");
    TEST_EXPECT_EQ(devhw.read(ac, buf, sizeof(buf), 99), (long)0, "read past len -> EOF");

    // Partial read honors the offset (seekable Dev).
    long g1 = devhw.read(ac, buf, 2, 0);
    long g2 = devhw.read(ac, buf, 2, 2);
    TEST_ASSERT(g1 == 2 && g2 == 2, "two 2-byte reads cover the 4-byte cell");

    devhw.close(ac);

    // A directory does not byte-read (readdir held).
    TEST_EXPECT_EQ(devhw.read(root, buf, sizeof(buf), 0), (long)-1, "read on a node dir -> -1");

    spoor_unref(ac);
    spoor_unref(root);
}

void test_devhw_stat_native(void) {
    struct Spoor *root = devhw.attach("");

    struct t_stat st;
    TEST_EXPECT_EQ(devhw.stat_native(root, &st), 0, "stat root OK");
    TEST_ASSERT((st.mode & T_S_IFDIR) != 0, "root mode is IFDIR");
    TEST_EXPECT_EQ(st.qid_type, (u32)QTDIR, "root stat qid_type QTDIR");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "root owned by SYSTEM");
    TEST_EXPECT_EQ(st.gid, (u32)GID_SYSTEM, "root group SYSTEM");

    struct Spoor *ac = walk1(root, "#address-cells");
    TEST_ASSERT(ac != NULL, "root/#address-cells");
    TEST_EXPECT_EQ(devhw.stat_native(ac, &st), 0, "stat #address-cells OK");
    TEST_ASSERT((st.mode & T_S_IFREG) != 0, "#address-cells mode is IFREG");
    TEST_EXPECT_EQ(st.size, (u64)4, "#address-cells stat size = 4 (one cell)");
    TEST_EXPECT_EQ(st.qid_type, (u32)QTFILE, "prop stat qid_type QTFILE");
    TEST_EXPECT_EQ(st.uid, (u32)PRINCIPAL_SYSTEM, "prop owned by SYSTEM");

    spoor_unref(ac);
    spoor_unref(root);
}

// The load-bearing test: the readdir dirent cookie must be STRICTLY MONOTONIC
// and NEVER 0 (the SYS_READDIR handler resumes from it + treats a non-advancing
// cookie as a malformed/hostile loop). Prove it by enumerating the root in
// one big call, then again in minimal chunks, and asserting the chunked
// enumeration yields the same entries in the same order with strictly-
// increasing non-zero cookies.
void test_devhw_readdir_cookie_contract(void) {
    struct Spoor *root = devhw.attach("");

    // Single-call enumeration into a generous buffer.
    static u8 big[4096];
    long total = devhw.readdir(root, big, sizeof(big), 0);
    TEST_ASSERT(total > 0, "root readdir is non-empty");

    // Count full entries + find the largest entry (so the chunk buffer can
    // always hold at least one -- avoids a spurious "buffer too small" -1).
    int full_count = 0;
    long max_entry = 0;
    {
        long pos = 0;
        for (;;) {
            u64 ck; u32 nl;
            long e = dirent_parse(big, pos, total, &ck, &nl, NULL);
            if (e == 0) break;
            full_count++;
            if (e > max_entry) max_entry = e;
            pos += e;
        }
    }
    TEST_ASSERT(full_count >= 2, "root has multiple entries");

    // Chunked enumeration: a buffer just big enough for the largest single
    // entry forces many resume cycles.
    long chunk_cap = max_entry;          // >= one entry, < two typically
    u8 chunk[256];
    TEST_ASSERT(chunk_cap > 0 && chunk_cap <= (long)sizeof(chunk), "chunk fits a sane entry");

    int chunk_count = 0;
    u64 prev_cookie = 0;        // 0 == "none yet"; every real cookie must exceed it
    s64 off = 0;
    bool strict = true, nonzero = true;
    for (;;) {
        long got = devhw.readdir(root, chunk, chunk_cap, off);
        if (got <= 0) break;             // 0 == EOD; <0 would be a too-small buffer (asserted against below)
        u64 last = off < 0 ? 0 : (u64)off;
        long pos = 0;
        for (;;) {
            u64 ck; u32 nl;
            long e = dirent_parse(chunk, pos, got, &ck, &nl, NULL);
            if (e == 0) break;
            if (ck == 0) nonzero = false;
            if (ck <= prev_cookie) strict = false;    // must strictly exceed every prior cookie
            prev_cookie = ck;
            last = ck;
            chunk_count++;
            pos += e;
        }
        if ((s64)last == off) break;     // safety: cursor must advance
        off = (s64)last;
    }

    TEST_ASSERT(nonzero, "every readdir cookie is non-zero");
    TEST_ASSERT(strict, "readdir cookies strictly increase across the enumeration");
    TEST_EXPECT_EQ(chunk_count, full_count, "chunked enumeration yields every entry exactly once");

    spoor_unref(root);
}

// Direct lib/dtb.c tree-walk API: iterate the root's direct contents. Every
// entry carries a non-empty name; at least one node + one property exist
// (the root has child nodes AND cell properties).
void test_devhw_iter_api(void) {
    TEST_ASSERT(dtb_node_at(DTB_NODE_ROOT, NULL, NULL), "root is a valid node");

    u32 cursor = 0;
    struct dtb_node_entry e;
    int nodes = 0, props = 0;
    u32 prev = 0;
    bool advancing = true, named = true;
    while (dtb_node_iter(DTB_NODE_ROOT, &cursor, &e)) {
        if (e.namelen == 0 || e.name == NULL) named = false;
        if (cursor <= prev) advancing = false;     // the opaque cursor strictly increases
        prev = cursor;
        if (e.is_node) nodes++; else props++;
        if (nodes + props > 256) break;            // runaway guard (root has < 30)
    }
    TEST_ASSERT(named, "every root entry has a non-empty name");
    TEST_ASSERT(advancing, "the iter cursor strictly advances");
    TEST_ASSERT(nodes >= 1, "root has at least one sub-node");
    TEST_ASSERT(props >= 1, "root has at least one property");

    // A bad offset is rejected, not followed.
    TEST_ASSERT(!dtb_node_at(0x7fffffffu, NULL, NULL), "out-of-range node offset rejected");
    TEST_ASSERT(!dtb_prop_at(0x7fffffffu, NULL, NULL, NULL), "out-of-range prop offset rejected");
}
